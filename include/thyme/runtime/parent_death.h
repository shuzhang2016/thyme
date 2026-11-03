#pragma once
#include <csignal>
#include <cstdint>

namespace thyme {

// OPS-02: parent-death signal for supervisor-spawned role processes.
//
// PR_SET_PDEATHSIG is cleared across fork(), so a child must arm it itself
// (before or after execv; the setting survives a non-setuid exec). There is a
// window between "read my parent" and "kernel arms the signal" in which the
// parent can die; after arming we therefore re-read getppid() and report
// kParentGone if it changed (or is already 1), telling the child to exit
// instead of running unsupervised.
enum class ParentDeathSignal : uint8_t {
  kInstalled = 0,  // signal armed, parent still the same live process
  kParentGone,     // parent died before/while arming: the child must exit
  kUnsupported,    // non-Linux: no-op, caller continues normally
};

// Arms PR_SET_PDEATHSIG(signo) for the calling process. Returns kUnsupported
// on non-Linux builds (no-op), so callers can treat it as a best-effort
// hardening step.
ParentDeathSignal InstallParentDeathSignal(int signo = SIGTERM) noexcept;

}  // namespace thyme
