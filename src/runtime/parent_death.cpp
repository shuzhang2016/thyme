#include "thyme/runtime/parent_death.h"

#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace thyme {

ParentDeathSignal InstallParentDeathSignal(int signo) noexcept {
#ifdef __linux__
  const pid_t parent_before = getppid();
  // Already reparented: the signal would never fire for the original parent.
  if (parent_before <= 1) return ParentDeathSignal::kParentGone;
  if (prctl(PR_SET_PDEATHSIG, static_cast<unsigned long>(signo), 0UL, 0UL,
            0UL) != 0) {
    return ParentDeathSignal::kUnsupported;
  }
  // Race with the parent's death: PR_SET_PDEATHSIG only fires on a *future*
  // transition, so a parent that died in the window above left us orphaned
  // and unsupervised. Detect the reparenting and let the caller exit.
  const pid_t parent_after = getppid();
  if (parent_after != parent_before || parent_after <= 1) {
    return ParentDeathSignal::kParentGone;
  }
  return ParentDeathSignal::kInstalled;
#else
  (void)signo;
  return ParentDeathSignal::kUnsupported;
#endif
}

}  // namespace thyme
