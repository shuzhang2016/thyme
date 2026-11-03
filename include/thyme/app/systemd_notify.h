#pragma once
// OPS-03: minimal sd_notify(3) shim for systemd Type=notify / WatchdogSec.
//
// thyme does not link libsystemd. A deployment that wants the systemd
// start-up handshake and watchdog enables THYME_SD_NOTIFY=1; systemd then
// sets NOTIFY_SOCKET and this shim sends plain AF_UNIX datagrams:
//
//   ForkRoles()    -> READY=1     (once every role has been forked)
//   PollRestarts() -> WATCHDOG=1  (at most once per second)
//
// Both calls are no-ops unless THYME_SD_NOTIFY is truthy *and* NOTIFY_SOCKET
// names a datagram socket. Nothing is allocated, the send is non-blocking,
// and a missing or broken socket is reported as false rather than fatal:
// lose the notification, not the dataflow.
//
// The watchdog proves that the supervisor poll loop is alive. It does not
// replace OPS-01: OPS-01 restarts one stalled role inside the live session
// (bounded backoff, SHM preserved), while systemd acts only when the whole
// supervisor stops polling and restarts the session (OPS-02 then reclaims
// stale SHM).
namespace thyme {
namespace app {

// True when THYME_SD_NOTIFY is 1/yes/true/on and NOTIFY_SOCKET is non-empty.
bool SystemdNotifyEnabled();

// Sends one datagram containing `state` (for example "READY=1"). Returns true
// when the datagram was handed to the kernel. False when disabled, when the
// address is invalid, or when the send fails.
bool SystemdNotifySend(const char* state);

bool SystemdNotifyReady();
bool SystemdNotifyWatchdog();

}  // namespace app
}  // namespace thyme
