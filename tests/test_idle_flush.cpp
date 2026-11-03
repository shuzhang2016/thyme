// test_idle_flush.cpp — IdleDetector semantics (design 6.2) with an injected
// predicate and fake clock: busy never flushes; sustained idle flushes after
// idle_hold; condition loss resets the hold timer; unreadable source follows
// idle_fallback (always_busy | assume_idle); quota pressure overrides busy.
#include "thyme/check.h"
#include "thyme/record/idle_detector.h"

#include <cstdint>

using namespace thyme;

namespace {

struct PredState {
  bool cond = false;
  bool ok = true;
};

bool Pred(void* ctx, bool* source_ok) {
  const PredState* s = static_cast<const PredState*>(ctx);
  *source_ok = s->ok;
  return s->cond;
}

constexpr uint64_t kHold = 1000000000ULL;  // 1 s (test scale)

idle::IdleDetector MakeDetector(PredState* s, bool fallback_idle = false,
                                double pressure = 0.8) {
  idle::Config cfg;
  cfg.idle_hold_ns = kHold;
  cfg.fallback_assume_idle = fallback_idle;
  cfg.pressure_release_ratio = pressure;
  return idle::IdleDetector(cfg, Pred, s);
}

}  // namespace

int main() {
  PredState st;

  // Busy: never flushes, no matter the pending ratio below the release point.
  {
    idle::IdleDetector d = MakeDetector(&st);
    st.cond = false;
    for (uint64_t t = 0; t < 10 * kHold; t += kHold / 10) {
      CHECK(!d.ShouldFlush(t, 0.5));
    }
  }

  // Sustained idle: flush only after the full hold; an interruption resets.
  {
    idle::IdleDetector d = MakeDetector(&st);
    st.cond = true;
    CHECK(!d.ShouldFlush(1000, 0.0));            // condition starts
    CHECK(d.condition_since() == 1000);
    CHECK(!d.ShouldFlush(1000 + kHold / 2, 0.0));
    st.cond = false;                             // busy again -> timer resets
    CHECK(!d.ShouldFlush(1000 + kHold, 0.0));
    CHECK(d.condition_since() == 0);
    st.cond = true;
    CHECK(!d.ShouldFlush(2000 + kHold, 0.0));    // restarted at 2000+...
    CHECK(d.condition_since() == 2000 + kHold);
    CHECK(d.ShouldFlush(2000 + 2 * kHold, 0.0));  // full hold -> flush
  }

  // Unreadable source, always_busy: no flush even with stale condition data.
  {
    idle::IdleDetector d = MakeDetector(&st, /*fallback_idle=*/false);
    st.cond = true;
    st.ok = true;
    CHECK(!d.ShouldFlush(1000, 0.0));
    st.ok = false;  // mirror unreadable
    CHECK(!d.ShouldFlush(1000 + 100 * kHold, 0.0));
  }

  // Unreadable source, assume_idle: flushes.
  {
    idle::IdleDetector d = MakeDetector(&st, /*fallback_idle=*/true);
    st.ok = false;
    CHECK(d.ShouldFlush(1, 0.0));
  }

  // Quota pressure override: busy but pending >= 80% of quota -> flush.
  {
    idle::IdleDetector d = MakeDetector(&st);
    st.ok = true;
    st.cond = false;  // busy
    CHECK(!d.ShouldFlush(1, 0.79));
    CHECK(d.ShouldFlush(1, 0.80));
    CHECK(d.ShouldFlush(1, 1.0));
  }
  return 0;
}
