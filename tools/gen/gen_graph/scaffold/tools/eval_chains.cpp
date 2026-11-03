// eval_chains: run the graph pipeline (in-process or via `ad_stack --mp`) and
// emit a JSON report built entirely from per-node/per-edge perf samples
// (latency + work) plus /proc resource sampling. No node-name literals: the
// chain and every node are resolved through the generated kGraphNodes table
// and the Pipeline node-id dispatchers.
//
// The JSON carries BOTH percentile summaries and per-sample time series
// (node_series / edge_series / work_series / e2e.series) so a renderer can
// draw the same time-series charts a human report expects.
//
// CLI:
//   --mode inproc|mp       inproc (default) runs the Pipeline directly and
//                          samples /proc/self; mp forks AD_STACK --mp, parses
//                          perf files, and samples the /proc/t9_* role procs
//   --seconds N            run duration per scenario (default 15)
//   --chain a,b,c          repeatable/csv chain (default
//                          camera,perception,fusion,loasd,arcomm)
//   --load L               stress load to run under: baseline (default,
//                          no load) | cpu | io | irq
//   [AD_STACK]             positional binary for --mode mp

#include "generated/graph_ir.h"
#include "generated/record_manifest.h"
#ifdef THYME_GRAPH_HAS_RECORD
#include "generated/record_registry.h"
#endif
#include "generated/wiring.h"
#include "thyme/record/record_schema.h"
#include "thyme/runtime/perf_log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <glob.h>
#include <map>
#include <string>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

// ---------------- perf wire parsing (T9P1) ----------------

// ---------------- perf wire parsing (T9P1) ----------------

// P2: sample/work timestamps are u32 (mono>>14, wrap ~19.5h); periodic dump
// files carry type-4 epoch blocks with the wrap base. ts expands to u64 units
// and bumps the base whenever the low word regresses within an epoch.
struct PerfNode {
  uint8_t node = 0;
  struct Sample {
    uint64_t ts;
    uint32_t lat;
    uint8_t io;
  };
  std::vector<Sample> samples;
  struct Work {
    uint64_t ts;
    uint32_t dur;
    uint8_t io, flags;
  };
  std::vector<Work> work;
  // epoch state (type-4 blocks)
  uint64_t epoch_base = 0;
  uint32_t last_ts_low = 0;

  uint64_t Expand(uint32_t low) {
    if (epoch_base != 0 && last_ts_low != 0 && low < last_ts_low) {
      epoch_base += 1ull << 32;  // wrap inside the epoch
    }
    last_ts_low = low;
    return epoch_base + low;
  }
};

uint32_t RdU24(const uint8_t* p) {
  return p[0] | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}
uint32_t RdU32(const uint8_t* p) {
  return p[0] | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t RdU64(const uint8_t* p) {
  return RdU32(p) | (static_cast<uint64_t>(RdU32(p + 4)) << 32);
}

// Parse a T9P1 buffer (magic + block headers) into an id-indexed map.
std::map<uint8_t, PerfNode> ParsePerfBuffer(const uint8_t* d, std::size_t size) {
  std::map<uint8_t, PerfNode> nodes;
  if (d == nullptr || size < 4 || std::memcmp(d, "T9P1", 4) != 0) return nodes;
  std::size_t p = 4;
  while (p + 6 <= size) {
    const uint8_t typ = d[p], node = d[p + 1];
    const uint32_t cnt = RdU32(&d[p + 2]);
    p += 6;
    // Skip blocks that carry no latency/work samples. Type 6 histogram blocks
    // name the sampler's default node id (a node index for edge samplers),
    // which is not a RecordStructId and must not materialize an edge.
    if (typ == 5) {  // proc-sampler series (OBS-04): 10B records
      p += 10 * cnt;
      continue;
    }
    if (typ == 6) {  // histogram block (OBS-09): kPerfHistRecordBytes each
      p += static_cast<std::size_t>(cnt) * thyme::kPerfHistRecordBytes;
      continue;
    }
    if (typ == thyme::kPerfSeriesNameBlockType) {
      p += static_cast<std::size_t>(cnt) * thyme::kPerfSeriesNameRecordBytes;
      continue;
    }
    if (typ == thyme::kPerfNodeBudgetBlockType) {
      p += static_cast<std::size_t>(cnt) * thyme::kPerfNodeBudgetRecordBytes;
      continue;
    }
    if (typ == 7) {  // ABI-10 record-manifest block: no samples
      // Ids in the perf stream are RecordStructIds, i.e. row indexes into the
      // GEN-12 recorded-channel list. Warn loudly (once per block) when the
      // log was produced by a different manifest; thyme_log_merge is the
      // analyser that refuses outright.
      for (uint32_t i = 0; i < cnt && p + thyme::kPerfManifestRecordBytes <= size;
           ++i, p += thyme::kPerfManifestRecordBytes) {
        const uint16_t ver = static_cast<uint16_t>(
            d[p] | (static_cast<uint16_t>(d[p + 1]) << 8));
        uint64_t hash = 0;
        for (int b = 0; b < 8; ++b) {
          hash |= static_cast<uint64_t>(d[p + 4 + b]) << (8 * b);
        }
        if (ver != thyme::schema::kManifestVersion || hash != kRecordManifestHash) {
          std::fprintf(stderr,
                       "eval_chains: perf log record manifest 0x%016llx v%u "
                       "differs from this build (0x%016llx v%u); "
                       "RecordStructId labels may be wrong\n",
                       static_cast<unsigned long long>(hash),
                       static_cast<unsigned>(ver),
                       static_cast<unsigned long long>(kRecordManifestHash),
                       static_cast<unsigned>(thyme::schema::kManifestVersion));
        }
      }
      continue;
    }
    PerfNode& pn = nodes[node];
    pn.node = node;
    if (typ == 1) {
      for (uint32_t i = 0; i < cnt && p + 10 <= size; ++i, p += 10) {
        pn.samples.push_back({pn.Expand(RdU32(&d[p])), RdU24(&d[p + 4]),
                              d[p + 8]});
      }
    } else if (typ == 2) {
      p += 20 * cnt;
    } else if (typ == 3) {
      for (uint32_t i = 0; i < cnt && p + 10 <= size; ++i, p += 10) {
        pn.work.push_back({pn.Expand(RdU32(&d[p])), RdU24(&d[p + 4]),
                           d[p + 7], d[p + 9]});
      }
    } else if (typ == 4) {  // epoch block (P2): u64 wrap base per record
      for (uint32_t i = 0; i < cnt && p + 8 <= size; ++i, p += 8) {
        pn.epoch_base = RdU64(&d[p]);
        pn.last_ts_low = 0;
      }
    } else {
      break;
    }
  }
  return nodes;
}
// ---------------- resource sampling (/proc/self, /proc/t9_*) ----------------

struct ResPoint {
  uint64_t t_ms = 0;
  std::string role;  // empty = self (inproc); mp role name otherwise
  uint64_t cpu_jif = 0;
  uint64_t rss_kb = 0;
};

uint64_t MonoMs() {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

ResPoint SampleSelf(uint64_t t_ms) {
  ResPoint s;
  s.t_ms = t_ms;
  char buf[1024];
  int fd = open("/proc/self/stat", O_RDONLY);
  if (fd >= 0) {
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
      buf[n] = '\0';
      const char* r = std::strrchr(buf, ')');
      if (r != nullptr) {
        char* save = nullptr;
        char* tok = strtok_r(const_cast<char*>(r + 2), " ", &save);
        unsigned long long utime = 0, stime = 0;
        for (int field = 3; tok != nullptr && field <= 15; ++field) {
          if (field == 14) utime = std::strtoull(tok, nullptr, 10);
          if (field == 15) stime = std::strtoull(tok, nullptr, 10);
          tok = strtok_r(nullptr, " ", &save);
        }
        s.cpu_jif = utime + stime;
      }
    }
  }
  fd = open("/proc/self/status", O_RDONLY);
  if (fd >= 0) {
    const ssize_t m = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (m > 0) {
      buf[m] = '\0';
      const char* vm = std::strstr(buf, "VmRSS:");
      if (vm != nullptr) s.rss_kb = std::strtoull(vm + 6, nullptr, 10);
    }
  }
  return s;
}

// Scan /proc for the mp role processes (comm t9_<role>): one point per role.
// Used in --mode mp while the forked `ad_stack --mp` supervisor is alive.
std::vector<ResPoint> ScanResources(uint64_t t_ms) {
  std::vector<ResPoint> out;
  DIR* dir = opendir("/proc");
  if (dir == nullptr) return out;
  struct dirent* de;
  while ((de = readdir(dir)) != nullptr) {
    if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
    char path[300], buf[1024];
    std::snprintf(path, sizeof(path), "/proc/%s/stat", de->d_name);
    int fd = open(path, O_RDONLY);
    if (fd < 0) continue;
    const ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) continue;
    buf[n] = '\0';
    const char* l = std::strchr(buf, '(');
    const char* r = std::strrchr(buf, ')');
    if (l == nullptr || r == nullptr || r - l < 2) continue;
    char comm[64];
    std::snprintf(comm, sizeof(comm), "%.*s", static_cast<int>(r - l - 1),
                  l + 1);
    if (std::strncmp(comm, "t9_", 3) != 0) continue;
    ResPoint s;
    s.t_ms = t_ms;
    s.role = comm + 3;
    char* p = const_cast<char*>(r + 2);
    char* save = nullptr;
    char* tok = strtok_r(p, " ", &save);
    unsigned long long utime = 0, stime = 0;
    for (int field = 3; tok != nullptr && field <= 15; ++field) {
      if (field == 14) utime = std::strtoull(tok, nullptr, 10);
      if (field == 15) stime = std::strtoull(tok, nullptr, 10);
      tok = strtok_r(nullptr, " ", &save);
    }
    s.cpu_jif = utime + stime;
    std::snprintf(path, sizeof(path), "/proc/%s/status", de->d_name);
    fd = open(path, O_RDONLY);
    if (fd >= 0) {
      const ssize_t m = read(fd, buf, sizeof(buf) - 1);
      close(fd);
      if (m > 0) {
        buf[m] = '\0';
        const char* vm = std::strstr(buf, "VmRSS:");
        if (vm != nullptr) s.rss_kb = std::strtoull(vm + 6, nullptr, 10);
      }
    }
    out.push_back(std::move(s));
  }
  closedir(dir);
  return out;
}

// ---------------- stats ----------------

uint64_t Pct(std::vector<uint32_t> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, static_cast<std::size_t>(v.size() * q))];
}

const char* NodeName(uint8_t id) {
  return id < kGraphNodesCount ? kGraphNodes[id].id : "?";
}

const char* EdgeName(uint8_t id) {
#ifdef THYME_GRAPH_HAS_RECORD
  return id < kRecordRegistryCount ? kRecordRegistry[id].channel : "?";
#else
  (void)id;
  return "?";
#endif
}

double JitterStd(const std::vector<PerfNode::Sample>& samples) {
  if (samples.size() < 3) return 0;
  double sum = 0, sq = 0;
  std::size_t n = 0;
  for (std::size_t i = 1; i < samples.size(); ++i) {
    const double dt =
        static_cast<double>(samples[i].ts - samples[i - 1].ts) * 16.384;  // µs
    sum += dt;
    sq += dt * dt;
    ++n;
  }
  const double mean = sum / n;
  return std::sqrt(sq / n - mean * mean);
}

// ---------------- end-to-end frame join ----------------

struct E2eResult {
  std::vector<std::pair<uint64_t, uint64_t>> series;  // (head_ts_ns, e2e_ns)
  uint64_t broken = 0;
};

const PerfNode* FindNode(const std::vector<PerfNode>& v, const char* name) {
  for (const auto& pn : v) {
    if (std::strcmp(NodeName(pn.node), name) == 0) return &pn;
  }
  return nullptr;
}

E2eResult JoinChain(const std::vector<PerfNode>& nodes,
                    const std::vector<std::string>& chain) {
  E2eResult res;
  if (chain.empty()) return res;
  const PerfNode* head = FindNode(nodes, chain[0].c_str());
  if (head == nullptr) return res;
  constexpr uint64_t kJoinWindowNs = 200000000ULL;
  for (const auto& w0 : head->work) {
    if (!(w0.flags & 0x2) || (w0.flags & 0x1)) continue;
    uint64_t prev_ts = static_cast<uint64_t>(w0.ts) << 14;
    uint8_t out_nib = w0.io >> 4;
    const PerfNode::Work* last = &w0;
    bool ok = true;
    for (std::size_t ci = 1; ci < chain.size(); ++ci) {
      const PerfNode* nd = FindNode(nodes, chain[ci].c_str());
      const bool is_end = (ci + 1 == chain.size());
      const PerfNode::Work* match = nullptr;
      if (nd != nullptr) {
        for (const auto& w : nd->work) {
          const uint64_t wts = static_cast<uint64_t>(w.ts) << 14;
          if (wts < prev_ts || wts - prev_ts > kJoinWindowNs) continue;
          if (!(w.flags & 0x4)) continue;
          if (w.flags & 0x1) continue;
          if ((w.io & 0xF) != out_nib) continue;
          if (!is_end && !(w.flags & 0x2)) continue;
          match = &w;
          break;
        }
      }
      if (match == nullptr) {
        ok = false;
        break;
      }
      prev_ts = static_cast<uint64_t>(match->ts) << 14;
      out_nib = match->io >> 4;
      last = match;
    }
    if (!ok) {
      ++res.broken;
      continue;
    }
    const uint64_t head_ts = static_cast<uint64_t>(w0.ts) << 14;
    const uint64_t end_ts = (static_cast<uint64_t>(last->ts) << 14) +
                            (static_cast<uint64_t>(last->dur) << 10);
    res.series.emplace_back(head_ts, end_ts >= head_ts ? end_ts - head_ts : 0);
  }
  return res;
}

// ---------------- stress loads ----------------

std::vector<pid_t> g_load_pids;
int g_load_requested = 0;
int g_load_alive = 0;

void StopLoads() {
  for (pid_t p : g_load_pids) {
    kill(p, SIGKILL);
    waitpid(p, nullptr, 0);
  }
  g_load_pids.clear();
}

void PinCpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  sched_setaffinity(0, sizeof(set), &set);
}

void SpinForever(int cpu) {
  PinCpu(cpu);
  volatile uint64_t x = 1;
  for (;;) x = x * 6364136223846793005ULL + 1;
}

void IoWriter(int) {
  char path[64];
  std::snprintf(path, sizeof(path), "/tmp/thyme_eval_io_%d",
                static_cast<int>(getpid()));
  std::vector<char> buf(64 * 1024, 0xAB);
  for (;;) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) _exit(1);
    for (int i = 0; i < 16; ++i) {
      if (write(fd, buf.data(), buf.size()) < 0) _exit(1);
    }
    fsync(fd);
    close(fd);
    unlink(path);
  }
}

void TimerStorm(int) {
  int fd = timerfd_create(CLOCK_MONOTONIC, 0);
  if (fd < 0) _exit(1);
  struct itimerspec its {};
  its.it_value.tv_nsec = 1000;
  its.it_interval.tv_nsec = 50000;
  timerfd_settime(fd, 0, &its, nullptr);
  uint64_t expirations;
  for (;;) {
    if (read(fd, &expirations, sizeof(expirations)) < 0) _exit(1);
  }
}

void YieldStorm(int) {
  for (;;) sched_yield();
}

pid_t ForkFn(void (*fn)(int), int arg) {
  pid_t p = fork();
  if (p == 0) {
    fn(arg);
    _exit(0);
  }
  return p;
}

void StartLoads(const std::string& name) {
  if (name == "cpu") {
    g_load_pids.push_back(ForkFn(SpinForever, 1));
    g_load_pids.push_back(ForkFn(SpinForever, 2));
    g_load_pids.push_back(ForkFn(SpinForever, 3));
  } else if (name == "io") {
    g_load_pids.push_back(ForkFn(IoWriter, 0));
    g_load_pids.push_back(ForkFn(IoWriter, 0));
  } else if (name == "irq") {
    g_load_pids.push_back(ForkFn(TimerStorm, 0));
    g_load_pids.push_back(ForkFn(YieldStorm, 0));
    g_load_pids.push_back(ForkFn(YieldStorm, 0));
  }
  g_load_requested = static_cast<int>(g_load_pids.size());
  g_load_alive = 0;
  if (g_load_requested == 0) return;
  // Give every load process one scheduling opportunity to fail (missing
  // timerfd, unwritable /tmp) before the measured window starts. A partial
  // load still produces a usable report; the JSON and stderr say so.
  struct timespec settle {0, 100000000L};
  nanosleep(&settle, nullptr);
  for (pid_t p : g_load_pids) {
    int status = 0;
    if (waitpid(p, &status, WNOHANG) == 0) {
      ++g_load_alive;
    }
  }
  if (g_load_alive == g_load_requested) {
    std::fprintf(stderr, "eval_chains: load=%s ready (%d processes)\n",
                 name.c_str(), g_load_alive);
  } else {
    std::fprintf(stderr,
                 "eval_chains: load=%s DEGRADED (%d/%d processes alive); "
                 "the scenario still runs, see docs/guides/eval-chains.md\n",
                 name.c_str(), g_load_alive, g_load_requested);
  }
}

// ---------------- flush one node's samplers ----------------
//
// Node latency + work are flushed keyed by graph node index; edge latency is
// flushed keyed by RecordStructId. They MUST go into separate writers: node
// ids (0..kGraphNodesCount) and RecordStructIds (0..kRecordRegistryCount)
// overlap, and mixing them would merge node/edge samples under one key.

void FlushNodePerf(Pipeline& p, std::size_t i, thyme::PerfLogWriter& w) {
  thyme::CyclicThread* ct = p.ThreadByNodeId(kGraphNodes[i].id);
  if (ct != nullptr) {
    thyme::WritePerfNodeBudget(w, static_cast<uint8_t>(i), ct->period_ns(),
                               ct->deadline_ns());
    ct->perf().Flush(w, static_cast<uint8_t>(i));
    ct->work_log().Flush(w, static_cast<uint8_t>(i));
    return;
  }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  thyme::EventThread* et = p.EventThreadByNodeId(kGraphNodes[i].id);
  if (et != nullptr) {
    thyme::WritePerfNodeBudget(w, static_cast<uint8_t>(i), et->period_ns(),
                               et->deadline_ns());
    et->perf().Flush(w, static_cast<uint8_t>(i));
    et->work_log().Flush(w, static_cast<uint8_t>(i));
  }
#endif
}

void FlushNodeEdge(Pipeline& p, std::size_t i, thyme::PerfLogWriter& w) {
  thyme::CyclicThread* ct = p.ThreadByNodeId(kGraphNodes[i].id);
  if (ct != nullptr) {
    ct->perf_edge().Flush(w, static_cast<uint8_t>(i));
    return;
  }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  thyme::EventThread* et = p.EventThreadByNodeId(kGraphNodes[i].id);
  if (et != nullptr) et->perf_edge().Flush(w, static_cast<uint8_t>(i));
#endif
}

// OBS-03: rings are rate-sized per node, so a writer accumulating ALL nodes
// must be sized for the sum of their ring capacities.
void AccumNodeCaps(Pipeline& p, std::size_t i, std::size_t* lat_cap,
                   std::size_t* work_cap, std::size_t* edge_cap) {
  thyme::CyclicThread* ct = p.ThreadByNodeId(kGraphNodes[i].id);
  if (ct != nullptr) {
    *lat_cap += ct->perf().capacity();
    *work_cap += ct->work_log().capacity();
    *edge_cap += ct->perf_edge().capacity();
    return;
  }
#ifdef THYME_GRAPH_HAS_EVENT_NODES
  thyme::EventThread* et = p.EventThreadByNodeId(kGraphNodes[i].id);
  if (et != nullptr) {
    *lat_cap += et->perf().capacity();
    *work_cap += et->work_log().capacity();
    *edge_cap += et->perf_edge().capacity();
  }
#endif
}

// ---------------- report emission ----------------

void EmitReport(const std::string& mode, const std::string& load, int seconds,
                const std::vector<std::string>& chain,
                const std::vector<PerfNode>& nodes,
                const std::vector<PerfNode>& edges,
                const std::vector<ResPoint>& res) {
  const E2eResult e2e = JoinChain(nodes, chain);

  // Timestamps are absolute (CLOCK_MONOTONIC >> 14); re-zero at the earliest
  // sample so every series shares a readable t_ms axis.
  uint64_t t0_ns = ~0ULL;
  auto acc_t0 = [&](const std::vector<PerfNode>& v) {
    for (const auto& pn : v) {
      for (const auto& sp : pn.samples) {
        t0_ns = std::min(t0_ns, static_cast<uint64_t>(sp.ts) << 14);
      }
      for (const auto& wk : pn.work) {
        t0_ns = std::min(t0_ns, static_cast<uint64_t>(wk.ts) << 14);
      }
    }
  };
  acc_t0(nodes);
  acc_t0(edges);
  for (const auto& sp : e2e.series) t0_ns = std::min(t0_ns, sp.first);
  if (t0_ns == ~0ULL) t0_ns = 0;

  std::printf(
      "{\n  \"mode\": \"%s\",\n  \"load\": \"%s\",\n  \"load_requested\": %d,"
      "\n  \"load_alive\": %d,\n  \"seconds\": %d,\n",
      mode.c_str(), load.c_str(), g_load_requested, g_load_alive, seconds);
  std::printf("  \"chain\": [");
  for (std::size_t i = 0; i < chain.size(); ++i) {
    std::printf("%s\"%s\"", i ? ", " : "", chain[i].c_str());
  }
  std::printf("],\n");

  // Per-node percentile summary (latency + work).
  std::printf("  \"nodes\": [");
  bool first = true;
  for (const auto& pn : nodes) {
    std::vector<uint32_t> lats;
    lats.reserve(pn.samples.size());
    for (const auto& sp : pn.samples) lats.push_back(sp.lat);
    std::vector<uint32_t> durs;
    uint64_t idle = 0;
    for (const auto& wk : pn.work) {
      if (wk.flags & 0x6) durs.push_back(wk.dur);
      else ++idle;
    }
    std::printf(
        "%s{\"node\": \"%s\", \"lat_n\": %zu, \"lat_p50\": %llu,"
        " \"lat_p95\": %llu, \"lat_p99\": %llu, \"lat_max\": %llu,"
        " \"work_n\": %zu, \"work_p50\": %llu, \"work_p95\": %llu,"
        " \"work_max\": %llu, \"idle\": %llu}",
        first ? "" : ",", NodeName(pn.node), lats.size(),
        static_cast<unsigned long long>(Pct(lats, 0.50)) << 13,
        static_cast<unsigned long long>(Pct(lats, 0.95)) << 13,
        static_cast<unsigned long long>(Pct(lats, 0.99)) << 13,
        static_cast<unsigned long long>(Pct(lats, 1.0)) << 13, durs.size(),
        static_cast<unsigned long long>(Pct(durs, 0.50)) << 10,
        static_cast<unsigned long long>(Pct(durs, 0.95)) << 10,
        static_cast<unsigned long long>(Pct(durs, 1.0)) << 10,
        static_cast<unsigned long long>(idle));
    first = false;
  }
  std::printf("],\n");

  // Per-edge percentile summary (RecordStructId → channel).
  std::printf("  \"edges\": [");
  first = true;
  for (const auto& pe : edges) {
    std::vector<uint32_t> lats;
    lats.reserve(pe.samples.size());
    for (const auto& sp : pe.samples) lats.push_back(sp.lat);
    std::printf("%s{\"channel\": \"%s\", \"count\": %zu, \"rate\": %.2f,"
                " \"lat_p50\": %llu, \"lat_p95\": %llu, \"lat_p99\": %llu,"
                " \"lat_max\": %llu, \"jit_std\": %.1f}",
                first ? "" : ",", EdgeName(pe.node), pe.samples.size(),
                static_cast<double>(pe.samples.size()) / seconds,
                static_cast<unsigned long long>(Pct(lats, 0.50)) << 13,
                static_cast<unsigned long long>(Pct(lats, 0.95)) << 13,
                static_cast<unsigned long long>(Pct(lats, 0.99)) << 13,
                static_cast<unsigned long long>(Pct(lats, 1.0)) << 13,
                JitterStd(pe.samples));
    first = false;
  }
  std::printf("],\n");

  // Frame-joined E2E: percentiles + per-frame series.
  std::vector<uint64_t> latv;
  latv.reserve(e2e.series.size());
  for (const auto& sp : e2e.series) latv.push_back(sp.second);
  std::sort(latv.begin(), latv.end());
  auto pct = [&](double q) -> uint64_t {
    return latv.empty() ? 0
                        : latv[std::min(latv.size() - 1,
                                        static_cast<std::size_t>(latv.size() * q))];
  };
  std::printf(
      "  \"e2e\": {\"n\": %zu, \"broken\": %llu, \"p50\": %llu,"
      " \"p95\": %llu, \"p99\": %llu, \"max\": %llu, \"series\": [",
      e2e.series.size(), static_cast<unsigned long long>(e2e.broken),
      static_cast<unsigned long long>(pct(0.50)),
      static_cast<unsigned long long>(pct(0.95)),
      static_cast<unsigned long long>(pct(0.99)),
      static_cast<unsigned long long>(pct(1.0)));
  for (std::size_t j = 0; j < e2e.series.size(); ++j) {
    std::printf("%s[%llu,%llu]", j == 0 ? "" : ",",
                static_cast<unsigned long long>((e2e.series[j].first - t0_ns) /
                                                1000000ULL),
                e2e.series[j].second / 1000ULL);
  }
  std::printf("]},\n");

  // node_series: { node: [[t_ms, lat_us], ...] }
  std::printf("  \"node_series\": {");
  first = true;
  for (const auto& pn : nodes) {
    std::printf("%s\"%s\": [", first ? "" : ",", NodeName(pn.node));
    for (std::size_t j = 0; j < pn.samples.size(); ++j) {
      const uint64_t abs_ns = static_cast<uint64_t>(pn.samples[j].ts) << 14;
      std::printf("%s[%llu,%llu]", j == 0 ? "" : ",",
                  static_cast<unsigned long long>((abs_ns - t0_ns) / 1000000ULL),
                  static_cast<unsigned long long>(
                      (static_cast<uint64_t>(pn.samples[j].lat) << 13) /
                      1000ULL));
    }
    std::printf("]");
    first = false;
  }
  std::printf("},\n");

  // edge_series: { channel: [[t_ms, lat_us], ...] }
  std::printf("  \"edge_series\": {");
  first = true;
  for (const auto& pe : edges) {
    std::printf("%s\"%s\": [", first ? "" : ",", EdgeName(pe.node));
    for (std::size_t j = 0; j < pe.samples.size(); ++j) {
      const uint64_t abs_ns = static_cast<uint64_t>(pe.samples[j].ts) << 14;
      std::printf("%s[%llu,%llu]", j == 0 ? "" : ",",
                  static_cast<unsigned long long>((abs_ns - t0_ns) / 1000000ULL),
                  static_cast<unsigned long long>(
                      (static_cast<uint64_t>(pe.samples[j].lat) << 13) /
                      1000ULL));
    }
    std::printf("]");
    first = false;
  }
  std::printf("},\n");

  // work_series: { node: [[t_ms, dur_us, io, flags], ...] }
  std::printf("  \"work_series\": {");
  first = true;
  for (const auto& pn : nodes) {
    if (pn.work.empty()) continue;
    std::printf("%s\"%s\": [", first ? "" : ",", NodeName(pn.node));
    for (std::size_t j = 0; j < pn.work.size(); ++j) {
      const auto& wk = pn.work[j];
      const uint64_t abs_ns = static_cast<uint64_t>(wk.ts) << 14;
      std::printf("%s[%llu,%llu,%u,%u]", j == 0 ? "" : ",",
                  static_cast<unsigned long long>((abs_ns - t0_ns) / 1000000ULL),
                  static_cast<unsigned long long>(
                      (static_cast<uint64_t>(wk.dur) << 10) / 1000ULL),
                  wk.io, wk.flags);
    }
    std::printf("]");
    first = false;
  }
  std::printf("},\n");

  // resources: flat [t_ms, cpu_jif, rss_kb] array for inproc self samples;
  // role-keyed object for mp (/proc/t9_* scans, same shape as the old tool).
  bool any_role = false;
  for (const auto& s : res) any_role = any_role || !s.role.empty();
  if (!any_role) {
    std::printf("  \"resources\": [");
    for (std::size_t i = 0; i < res.size(); ++i) {
      std::printf("%s[%llu,%llu,%llu]", i ? ", " : "",
                  static_cast<unsigned long long>(res[i].t_ms),
                  static_cast<unsigned long long>(res[i].cpu_jif),
                  static_cast<unsigned long long>(res[i].rss_kb));
    }
    std::printf("]\n}\n");
    return;
  }
  std::map<std::string, std::vector<const ResPoint*>> by_role;
  for (const auto& s : res) {
    by_role[s.role.empty() ? "self" : s.role].push_back(&s);
  }
  std::printf("  \"resources\": {");
  bool fr = true;
  for (const auto& [role, v] : by_role) {
    std::printf("%s\"%s\": [", fr ? "" : ",", role.c_str());
    for (std::size_t i = 0; i < v.size(); ++i) {
      std::printf("%s[%llu,%llu,%llu]", i ? ", " : "",
                  static_cast<unsigned long long>(v[i]->t_ms),
                  static_cast<unsigned long long>(v[i]->cpu_jif),
                  static_cast<unsigned long long>(v[i]->rss_kb));
    }
    std::printf("]");
    fr = false;
  }
  std::printf("}\n}\n");
}

// Parse all files matching `pattern` (a glob) into a flat PerfNode vector.
std::vector<PerfNode> ParsePerfFiles(const std::string& pattern) {
  std::vector<PerfNode> nodes;
  glob_t g{};
  if (glob(pattern.c_str(), 0, nullptr, &g) != 0) return nodes;
  for (std::size_t fi = 0; fi < g.gl_pathc; ++fi) {
    FILE* f = std::fopen(g.gl_pathv[fi], "rb");
    if (f == nullptr) continue;
    std::vector<uint8_t> d;
    uint8_t chunk[8192];
    std::size_t r;
    while ((r = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      d.insert(d.end(), chunk, chunk + r);
    }
    std::fclose(f);
    std::map<uint8_t, PerfNode> m = ParsePerfBuffer(d.data(), d.size());
    for (auto& [id, pn] : m) nodes.push_back(std::move(pn));
  }
  globfree(&g);
  return nodes;
}

}  // namespace

int main(int argc, char** argv) {
  std::string mode = "inproc";
  int seconds = 15;
  std::vector<std::string> chain = {"camera", "perception", "fusion", "loasd",
                                    "arcomm"};
  bool chain_set = false;
  std::string load = "baseline";
  const char* ad_stack = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      mode = argv[++i];
    } else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atoi(argv[++i]);
      if (seconds <= 0) seconds = 15;
    } else if (std::strcmp(argv[i], "--load") == 0 && i + 1 < argc) {
      load = argv[++i];
    } else if (std::strcmp(argv[i], "--chain") == 0 && i + 1 < argc) {
      if (!chain_set) {
        chain.clear();
        chain_set = true;
      }
      const char* csv = argv[++i];
      const char* p = csv;
      while (*p != '\0') {
        while (*p == ',' || *p == ' ') ++p;
        if (*p == '\0') break;
        const char* start = p;
        while (*p != '\0' && *p != ',') ++p;
        const char* end = p;
        while (end > start && end[-1] == ' ') --end;
        if (end > start) chain.emplace_back(start, end - start);
      }
    } else if (argv[i][0] != '-') {
      ad_stack = argv[i];
    } else {
      std::fprintf(stderr,
                   "usage: %s [--mode inproc|mp] [--seconds N] "
                   "[--chain a,b,c] [--load baseline|cpu|io|irq] [AD_STACK]\n",
                   argv[0]);
      return 2;
    }
  }
  if (mode != "inproc" && mode != "mp") {
    std::fprintf(stderr, "eval_chains: --mode must be inproc|mp\n");
    return 2;
  }
  if (load != "baseline" && load != "cpu" && load != "io" && load != "irq") {
    std::fprintf(stderr, "eval_chains: --load must be baseline|cpu|io|irq\n");
    return 2;
  }

  if (mode == "inproc") {
    std::vector<ResPoint> res;
    const uint64_t t0 = MonoMs();
    auto p = Pipeline::Create();
    p->Start();
    StartLoads(load);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      res.push_back(SampleSelf(MonoMs() - t0));
    }
    p->RequestStop();
    p->Join();
    StopLoads();
    res.push_back(SampleSelf(MonoMs() - t0));

    // Node + edge perf must stay in separate writers (id spaces overlap).
    // OBS-03: size each writer for the sum of the ring capacities it
    // accumulates (rings are rate-sized at thread construction).
    std::size_t lat_cap = 0, work_cap = 0, edge_cap = 0;
    for (std::size_t i = 0; i < kGraphNodesCount; ++i) {
      AccumNodeCaps(*p, i, &lat_cap, &work_cap, &edge_cap);
    }
    thyme::PerfLogWriter w(lat_cap, work_cap), we(edge_cap, 0);
    for (std::size_t i = 0; i < kGraphNodesCount; ++i) FlushNodePerf(*p, i, w);
    for (std::size_t i = 0; i < kGraphNodesCount; ++i) FlushNodeEdge(*p, i, we);

    std::vector<PerfNode> nodes, edges;
    for (const auto& [id, pn] : ParsePerfBuffer(w.data(), w.size())) {
      nodes.push_back(pn);
    }
    for (const auto& [id, pn] : ParsePerfBuffer(we.data(), we.size())) {
      edges.push_back(pn);
    }
    EmitReport(mode, load, seconds, chain, nodes, edges, res);
    return 0;
  }

  // mp mode: fork AD_STACK --mp, then parse its perf files.
  if (ad_stack == nullptr) {
    std::fprintf(stderr, "eval_chains: --mode mp requires the AD_STACK binary\n");
    return 2;
  }
  char perf_prefix[160];
  std::snprintf(perf_prefix, sizeof(perf_prefix), "/tmp/t9perf_eval_%d",
                static_cast<int>(getpid()));
  setenv("T9_PERF_LOG", perf_prefix, 1);
  std::string sec = std::to_string(seconds);
  char* app_argv[] = {const_cast<char*>(ad_stack), const_cast<char*>("--mp"),
                      const_cast<char*>("--seconds"),
                      const_cast<char*>(sec.c_str()), nullptr};
  StartLoads(load);
  usleep(200 * 1000);  // let the load procs settle before the run starts
  pid_t app = fork();
  if (app == 0) {
    execv(ad_stack, app_argv);
    _exit(127);
  }
  // Sample the mp role processes (/proc/t9_*) while the supervisor runs.
  const uint64_t run_t0 = MonoMs();
  std::vector<ResPoint> res;
  int app_rc = -1;
  for (;;) {
    int st = 0;
    if (waitpid(app, &st, WNOHANG) == app) {
      app_rc = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
      break;
    }
    std::vector<ResPoint> batch = ScanResources(MonoMs() - run_t0);
    res.insert(res.end(), batch.begin(), batch.end());
    usleep(200 * 1000);
  }
  StopLoads();
  if (app_rc != 0) {
    std::fprintf(stderr, "eval_chains: ad_stack --mp exited %d\n", app_rc);
  }

  char pat[192];
  std::snprintf(pat, sizeof(pat), "%s_node_*.bin", perf_prefix);
  const std::vector<PerfNode> nodes = ParsePerfFiles(pat);
  std::snprintf(pat, sizeof(pat), "%s_edge_*.bin", perf_prefix);
  const std::vector<PerfNode> edges = ParsePerfFiles(pat);
  EmitReport(mode, load, seconds, chain, nodes, edges, res);
  return 0;
}
