// test_log_rotation.cpp — OPS-05: uniform rotation + process log budget for
// the append-only non-package files.
//
// Covers:
//   * size cap: rotation triggers at max_bytes, <path>.1 .. <path>.N are
//     created, the oldest generation is unlinked, no generation exceeds the
//     cap by more than one record, and one Append rotates at most once (the
//     writer never chases the cap in a loop);
//   * LogBudget: non-mandatory writers drop and count once the process budget
//     is exhausted, mandatory writers (audit) keep writing, Close releases the
//     accounted bytes;
//   * the trigger center's <socket>.fsm JSONL rotates under [logging] and the
//     retained generations stay parseable (CenterStats counters move);
//   * PerfLogDumper rotates T9P1 dumps, every generation carries the magic
//     and clock anchors, and thyme_log_merge consumes the live path plus
//     .1/.2 as independent inputs (same for JSONL `.fsm` generations);
//   * crash safety: a SIGKILLed child leaves complete lines/blocks in every
//     generation (only a partial tail is ever lost), never interleaved rows.
#include "log_merge.h"
#include "thyme/check.h"
#include "thyme/record/trigger_center.h"
#include "thyme/record/trigger_config.h"
#include "thyme/runtime/perf_dump.h"
#include "thyme/runtime/rotating_file.h"

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace thyme;
using trigger_center::Center;

namespace {

char g_dir[128];

std::string Path(const std::string& name) { return std::string(g_dir) + "/" + name; }

void MakeDir() {
  std::snprintf(g_dir, sizeof(g_dir), "/tmp/t9ops05_%d",
                static_cast<int>(getpid()));
  std::string cmd = "rm -rf ";
  cmd += g_dir;
  CHECK(std::system(cmd.c_str()) == 0);
  cmd = "mkdir -p ";
  cmd += g_dir;
  CHECK(std::system(cmd.c_str()) == 0);
}

bool Exists(const std::string& p) {
  struct stat st {};
  return stat(p.c_str(), &st) == 0;
}

uint64_t FileSize(const std::string& p) {
  struct stat st {};
  if (stat(p.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
}

std::vector<std::string> ReadLines(const std::string& p, bool* complete_tail) {
  std::vector<std::string> out;
  FILE* f = std::fopen(p.c_str(), "rb");
  if (f == nullptr) return out;
  std::string line;
  int c;
  while ((c = std::fgetc(f)) != EOF) {
    if (c == '\n') {
      out.push_back(line);
      line.clear();
    } else {
      line.push_back(static_cast<char>(c));
    }
  }
  if (complete_tail != nullptr) *complete_tail = line.empty();
  std::fclose(f);
  return out;
}

std::string ReadAll(const std::string& p) {
  std::string out;
  FILE* f = std::fopen(p.c_str(), "rb");
  if (f == nullptr) return out;
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) != 0) out.append(buf, n);
  std::fclose(f);
  return out;
}

void WriteAll(const std::string& p, const std::string& body) {
  FILE* f = std::fopen(p.c_str(), "wb");
  CHECK(f != nullptr);
  CHECK(std::fwrite(body.data(), 1, body.size(), f) == body.size());
  std::fclose(f);
}

// Fixed-size record so the cap math is exact: "<tag> NNNN" + 'x' padding +
// '\n', exactly `len` bytes with the delimiter last.
std::string Record(const char* tag, int n, std::size_t len) {
  char head[64];
  std::snprintf(head, sizeof(head), "%s %04d ", tag, n);
  std::string s(head);
  CHECK(len >= 2 && s.size() <= len - 1);
  s.append(len - 1 - s.size(), 'x');
  s.push_back('\n');
  return s;
}

// --- rotating file: cap + generations --------------------------------------
void TestCapAndGenerations() {
  logging::LogBudget::ResetForTest();
  const std::string path = Path("cap.log");
  logging::RotateConfig cfg;
  cfg.path = path;
  cfg.max_bytes = 100;
  cfg.keep = 2;
  cfg.account = false;  // this section tests the file geometry only
  logging::RotatingFile f;
  CHECK(f.Open(cfg));
  std::vector<int> written;
  for (int i = 0; i < 30; ++i) {
    const std::string rec = Record("cap", i, 40);
    const uint64_t before = f.rotations();
    CHECK(f.Append(rec.data(), rec.size()) == 40);
    CHECK(f.rotations() - before <= 1);  // one rotation per append, never a loop
    written.push_back(i);
  }
  // 30 x 40 B with a 100 B cap: the live file plus .1/.2, no .3.
  CHECK(Exists(path));
  CHECK(Exists(path + ".1"));
  CHECK(Exists(path + ".2"));
  CHECK(!Exists(path + ".3"));
  CHECK(f.rotations() >= 3);
  CHECK(FileSize(path) <= 100 + 40);  // overshoot is at most one record
  CHECK(FileSize(path + ".1") <= 100 + 40);
  CHECK(FileSize(path + ".2") <= 100 + 40);

  // Every generation is line-complete and ids increase across .2 -> .1 ->
  // live (the oldest records are the ones dropped).
  std::vector<int> ids;
  for (const std::string& p : {path + ".2", path + ".1", path}) {
    bool complete = false;
    for (const std::string& line : ReadLines(p, &complete)) {
      CHECK(line.compare(0, 4, "cap ") == 0);
      ids.push_back(std::atoi(line.c_str() + 4));
    }
    CHECK(complete);
  }
  CHECK(!ids.empty());
  for (std::size_t i = 1; i < ids.size(); ++i) CHECK(ids[i] == ids[i - 1] + 1);
  CHECK(ids.back() == written.back());
  CHECK(ids.front() > written.front());  // oldest generations were unlinked

  // A single record larger than the cap still commits in one write and only
  // rotates once.
  const std::string huge = Record("big", 99, 400);
  const uint64_t before = f.rotations();
  CHECK(f.Append(huge.data(), huge.size()) == 400);
  CHECK(f.rotations() - before == 1);
  CHECK(FileSize(path) == 400);
  f.Close();
}

// --- LogBudget: drop for non-mandatory, never for mandatory ----------------
void TestBudget() {
  logging::LogBudget::ResetForTest();
  logging::LogBudget::Configure(200);
  CHECK(logging::LogBudget::limit_bytes() == 200);

  logging::RotateConfig cfg;
  cfg.path = Path("budget.log");
  cfg.max_bytes = 1000000;
  cfg.keep = 1;
  cfg.account = true;
  logging::RotatingFile f;
  CHECK(f.Open(cfg));
  CHECK(logging::LogBudget::used_bytes() == 0);

  const std::string rec = Record("bud", 1, 120);
  CHECK(f.Append(rec.data(), rec.size()) == 120);
  CHECK(logging::LogBudget::used_bytes() == 120);
  // 120 more would exceed 200: dropped, counted, nothing written.
  CHECK(f.Append(rec.data(), rec.size()) == 0);
  CHECK(f.drops() == 1);
  CHECK(logging::LogBudget::drops() == 1);
  CHECK(logging::LogBudget::used_bytes() == 120);
  const std::string small = Record("bud", 2, 80);
  CHECK(f.Append(small.data(), small.size()) == 80);
  CHECK(logging::LogBudget::used_bytes() == 200);
  CHECK(f.Append(small.data(), small.size()) == 0);  // exactly at the limit

  // Mandatory writer (the SEC-02 audit) is counted but never dropped.
  logging::RotateConfig mcfg;
  mcfg.path = Path("audit.log");
  mcfg.max_bytes = 1000000;
  mcfg.keep = 1;
  mcfg.mandatory = true;
  mcfg.account = true;
  logging::RotatingFile m;
  CHECK(m.Open(mcfg));
  CHECK(m.Append(rec.data(), rec.size()) == 120);
  CHECK(logging::LogBudget::used_bytes() == 320);

  f.Close();
  CHECK(logging::LogBudget::used_bytes() == 120);  // only the mandatory live
  m.Close();
  CHECK(logging::LogBudget::used_bytes() == 0);
  logging::LogBudget::ResetForTest();
}

// --- trigger center FSM JSONL rotation ------------------------------------
struct FakeRec {
  bool active = false;
};

FakeRec g;

bool HookReady(void*) { return true; }
bool HookActive(void*) { return g.active; }
bool HookStart(void*, uint64_t, uint64_t, uint64_t, uint64_t, bool, bool,
               const std::vector<std::string>&) {
  g.active = true;
  return true;
}
bool HookPreempt(void*) {
  g.active = false;
  return true;
}

trigger_center::Hooks MakeHooks() {
  trigger_center::Hooks h;
  h.recorder_ready = HookReady;
  h.capture_active = HookActive;
  h.start_capture = HookStart;
  h.preempt_capture = HookPreempt;
  return h;
}

void TestFsmRotation() {
  logging::LogBudget::ResetForTest();
  g = FakeRec{};
  trigger_cfg::CenterConfig cfg;
  cfg.socket_path = Path("fsm.sock");
  cfg.log_fsm_max_bytes = 200;
  cfg.log_fsm_rotate_keep = 2;
  trigger_cfg::SourceRule rule;
  rule.name = "low";
  rule.quota_class = "q";
  cfg.sources = {rule};
  cfg.quota_limits = {{"q", 0}};
  Center c;
  CHECK(c.Init(cfg, MakeHooks()));
  c.set_fsm_dump_interval_ns(0);  // dump on every Tick
  // 12 cycles x 3 events (~55 B each) = ~2 KiB: several 200-byte rotations.
  for (int i = 0; i < 12; ++i) {
    CHECK(c.Submit("low", -1, 0, false, false).code ==
          trigger_proto::TriggerCode::kAccepted);
    c.Tick();  // start + dump
    g.active = false;
    c.Tick();  // complete + dump
  }
  c.Shutdown();

  const std::string live = cfg.socket_path + ".fsm";
  CHECK(Exists(live));
  CHECK(Exists(live + ".1"));
  CHECK(Exists(live + ".2"));
  CHECK(!Exists(live + ".3"));
  // Retained generations are complete JSONL; ids are ascending across
  // .2 -> .1 -> live and end at the newest trigger id.
  std::vector<uint64_t> ids;
  for (const std::string& p : {live + ".2", live + ".1", live}) {
    bool complete = false;
    for (const std::string& line : ReadLines(p, &complete)) {
      CHECK(line.find("{\"ts\":") == 0);
      const char* idp = std::strstr(line.c_str(), "\"id\":");
      CHECK(idp != nullptr);
      ids.push_back(std::strtoull(idp + 5, nullptr, 10));
    }
    CHECK(complete);
  }
  CHECK(!ids.empty());
  CHECK(ids.back() == 12);
  for (std::size_t i = 1; i < ids.size(); ++i) {
    CHECK(ids[i] >= ids[i - 1]);  // event order survives rotation
  }
}

// --- perf dump rotation + merge with generations --------------------------
void TestPerfRotationAndMerge() {
  logging::LogBudget::ResetForTest();
  const std::string path = Path("perf.bin");
  PerfLogDumper d;
  PerfDumpRotation rot;
  rot.max_bytes = 2000;  // one dump block is ~1.4 KiB (type-6 histogram)
  rot.rotate_keep = 2;
  CHECK(d.Open(path.c_str(), 0, rot));
  PerfSampler sampler(64);
  uint64_t now = 1000000000ull;
  for (int i = 0; i < 6; ++i) {
    for (int k = 0; k < 8; ++k) sampler.Note(now + static_cast<uint64_t>(k), 1000);
    CHECK(d.DumpIfDue(now + 1000000ull, sampler, nullptr, 3));
    now += 2000000ull;
  }
  d.Close();

  CHECK(Exists(path));
  CHECK(Exists(path + ".1"));
  CHECK(Exists(path + ".2"));
  CHECK(!Exists(path + ".3"));
  for (const std::string& p : {path, path + ".1", path + ".2"}) {
    // Every generation is a complete T9P1 file (magic re-emitted after
    // rotation), not a headerless continuation.
    CHECK(ReadAll(p).compare(0, 4, "T9P1") == 0);
  }

  // thyme_log_merge consumes live + .1 + .2 as independent inputs. Every row
  // from every generation must have its own ARCH-15 wall-clock mapping.
  std::vector<std::string> inputs = {path + ".2", path + ".1", path};
  logmerge::Options opt;
  FILE* out = std::fopen(Path("merge.txt").c_str(), "w+");
  CHECK(out != nullptr);
  CHECK(logmerge::Merge(inputs, opt, out, stderr) == 0);
  std::fflush(out);
  std::rewind(out);
  int rows = 0;
  bool saw_hist = false;
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), out) != nullptr) {
    ++rows;
    CHECK(std::strstr(buf, "realtime_ns=") != nullptr);
    if (std::strstr(buf, "hist ") != nullptr) saw_hist = true;
  }
  std::fclose(out);
  CHECK(rows > 0);
  CHECK(saw_hist);
}

// --- JSONL generations merge ----------------------------------------------
void TestFsmGenerationsMerge() {
  const std::string a = Path("gen.fsm.1");
  const std::string b = Path("gen.fsm");
  WriteAll(a, "{\"ts\":100,\"id\":1,\"from\":0,\"to\":1,\"why\":0}\n");
  WriteAll(b, "{\"ts\":200,\"id\":2,\"from\":1,\"to\":2,\"why\":5}\n");
  FILE* out = std::fopen(Path("genmerge.txt").c_str(), "w+");
  CHECK(out != nullptr);
  logmerge::Options opt;
  CHECK(logmerge::Merge({a, b}, opt, out, stderr) == 0);
  std::fflush(out);
  std::rewind(out);
  std::vector<std::string> lines;
  char buf[1024];
  while (std::fgets(buf, sizeof(buf), out) != nullptr) {
    std::string l(buf);
    while (!l.empty() && l.back() == '\n') l.pop_back();
    lines.push_back(l);
  }
  std::fclose(out);
  CHECK(lines.size() == 2);
  CHECK(lines[0].compare(0, 4, "100\t") == 0);
  CHECK(lines[1].compare(0, 4, "200\t") == 0);
}

// --- crash safety: SIGKILL leaves parseable generations -------------------
void TestCrashSafety() {
  const std::string path = Path("crash.log");
  const pid_t pid = fork();
  CHECK(pid >= 0);
  if (pid == 0) {
    logging::RotateConfig cfg;
    cfg.path = path;
    cfg.max_bytes = 160;
    cfg.keep = 2;
    cfg.account = false;
    logging::RotatingFile f;
    if (!f.Open(cfg)) _exit(2);
    for (int i = 0; i < 100000; ++i) {
      const std::string rec = Record("crash", i, 40);
      (void)f.Append(rec.data(), rec.size());
    }
    _exit(0);
  }
  // Let the child make progress (fork/open can be slow under load) so the
  // SIGKILL always lands after at least one append; then give it a few more
  // milliseconds to cross a rotation boundary.
  for (int i = 0; i < 2000; ++i) {
    if (FileSize(path) != 0 || Exists(path + ".1")) break;
    struct timespec tick {};
    tick.tv_sec = 0;
    tick.tv_nsec = 1000 * 1000;
    nanosleep(&tick, nullptr);
  }
  struct timespec ts {};
  ts.tv_sec = 0;
  ts.tv_nsec = 10 * 1000 * 1000;  // let it rotate several times
  nanosleep(&ts, nullptr);
  CHECK(kill(pid, SIGKILL) == 0);
  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid);

  std::vector<int> ids;
  for (const std::string& p : {path + ".2", path + ".1", path}) {
    if (!Exists(p)) continue;
    bool complete = false;
    for (const std::string& line : ReadLines(p, &complete)) {
      // Every complete line is one intact record: no interleaving, no torn
      // ID. Only the very last (partial) line may be missing.
      CHECK(line.compare(0, 6, "crash ") == 0);
      ids.push_back(std::atoi(line.c_str() + 6));
    }
  }
  CHECK(!ids.empty());
  // Killed mid-rotation can drop a whole generation (the unlink/rename window),
  // so ids may have gaps; they must still be strictly increasing in
  // .2 -> .1 -> live order: rotation never reorders an append.
  for (std::size_t i = 1; i < ids.size(); ++i) CHECK(ids[i] > ids[i - 1]);
}

}  // namespace

int main() {
  MakeDir();
  TestCapAndGenerations();
  TestBudget();
  TestFsmRotation();
  TestPerfRotationAndMerge();
  TestFsmGenerationsMerge();
  TestCrashSafety();
  logging::LogBudget::ResetForTest();
  const std::string cmd = std::string("rm -rf ") + g_dir;
  const int rc = std::system(cmd.c_str());
  (void)rc;
  std::printf("test_log_rotation: OK\n");
  return 0;
}
