#pragma once

// idle_detector.h — idle-flush decision for the settle gate (design 6.2).
//
// The idle CONDITION source is an injected predicate: production reads the
// chassis mirror (park_brk engaged && vehicle_speed < 0.1 m/s); tests and the
// demo host inject a simulated source. The detector adds the sustained-hold
// timing, the unreadable-source fallback (idle_fallback:
// always_busy|assume_idle), and the quota-pressure override (pending ratio >=
// pressure_release flushes even while busy — the design 6.2 safety net that
// keeps the pending queue bounded).
//
// Header-only, clock injected per call; no threads, no global state.

#include "thyme/record/record_mirror_ring_client.h"
#include "thyme/runtime/record_mirror_ring.h"

#include <cstdint>
#include <cstring>
#include <string>

namespace thyme {
namespace idle {

struct Config {
  uint64_t idle_hold_ns = 30000000000ull;  // hold duration (idle_hold_s, default 30s)
  bool fallback_assume_idle = false;  // idle_fallback: false=always_busy
  double pressure_release_ratio = 0.8;  // release once pending/quota reaches this ratio
};

// Returns the current idle-condition reading; sets *source_ok = false when
// the mirror is unreadable (fallback then applies).
using Predicate = bool (*)(void* ctx, bool* source_ok);

class IdleDetector {
 public:
  IdleDetector(Config cfg, Predicate pred, void* ctx)
      : cfg_(cfg), pred_(pred), ctx_(ctx) {}

  // Call once per host tick while a capture waits in deferred settle.
  bool ShouldFlush(uint64_t now_ns, double pending_ratio) {
    // Quota-pressure safety net: even busy traffic must not blow up the
    // pending queue.
    if (pending_ratio >= cfg_.pressure_release_ratio) return true;
    bool source_ok = true;
    const bool cond = pred_ != nullptr && pred_(ctx_, &source_ok);
    if (!source_ok) {
      // Mirror unreadable: fallback policy; the hold timer is kept as-is (it
      // is not restarted from the degraded state).
      return cfg_.fallback_assume_idle;
    }
    if (!cond) {
      cond_since_ns_ = 0;
      return false;
    }
    if (cond_since_ns_ == 0) cond_since_ns_ = now_ns;
    return now_ns - cond_since_ns_ >= cfg_.idle_hold_ns;
  }

  // Test/inspection: current sustained-condition start (0 = not met).
  uint64_t condition_since() const { return cond_since_ns_; }

 private:
  Config cfg_;
  Predicate pred_;
  void* ctx_;
  uint64_t cond_since_ns_ = 0;
};

// T3: production predicate — reads the chassis struct's latest sample from a
// mirror ring page and evaluates EPB-engaged && speed < threshold. Offsets
// are into that Frame sample. gen_graph fills them from the contract layout
// when the chassis type is recorded; otherwise they stay configuration.
// An unreadable page/sample reports source_ok=false (idle_fallback applies).
struct ChassisSpec {
  std::string page_shm;      // mirror ring page shm name
  uint16_t struct_id = 0;
  uint32_t epb_offset = 0;     // uint8 park_brk_status (!=0 => EPB Engaged)
  uint32_t speed_offset = 4;   // float vehicle_speed (m/s)
  double speed_threshold = 0.1;
};

class ChassisMirrorPredicate {
 public:
  bool Init(const ChassisSpec& spec, uint64_t timeout_ns = 2000000000ULL) {
    spec_ = spec;
    if (!client_.AttachReadOnly(spec_.page_shm.c_str(), timeout_ns)) {
      return false;
    }
    const RecordMirrorRingPage* page = client_.Page();
    return page != nullptr && page->Validate() &&
           spec_.struct_id < page->struct_count();
  }

  static bool Pred(void* ctx, bool* source_ok) {
    return static_cast<ChassisMirrorPredicate*>(ctx)->Eval(source_ok);
  }

  bool Eval(bool* source_ok) {
    const RecordMirrorRingPage* page = client_.Page();
    *source_ok = false;
    if (page == nullptr || !page->Validate()) return false;
    const uint64_t head = page->RingHead(spec_.struct_id);
    if (head == 0) return false;
    uint8_t buf[512];
    uint32_t sz = 0;
    uint64_t mono = 0;
    uint32_t sseq = 0;
    const uint32_t need = static_cast<uint32_t>(
        (spec_.epb_offset > spec_.speed_offset + sizeof(float)
             ? spec_.epb_offset + 1
             : spec_.speed_offset + sizeof(float)));
    if (need > sizeof(buf)) return false;
    if (!page->ReadRingSample(spec_.struct_id, head - 1, buf, need, sz, mono,
                              sseq)) {
      return false;
    }
    *source_ok = true;
    const uint8_t epb = buf[spec_.epb_offset];
    float speed = 0;
    std::memcpy(&speed, buf + spec_.speed_offset, sizeof(speed));
    return epb != 0 && speed < spec_.speed_threshold;
  }

 private:
  ChassisSpec spec_;
  record_mirror::RingClient client_;
};

}  // namespace idle
}  // namespace thyme
