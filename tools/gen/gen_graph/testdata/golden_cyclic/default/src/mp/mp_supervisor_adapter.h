#pragma once

// App-side supervisor playbook: runs --mp by composing the
// thyme::app::MpSupervisorSession skeleton (SHM set, mirror, fork/exec,
// cooperative stop, reap, collect) with this project's policy — run duration,
// trigger-relay polling, success-gate composition, the printf report, and the
// exit code. The config is built from the generated graph tables in
// mp_supervisor_adapter.cpp.
int RunSupervisor(const char* self_exe, int seconds, bool record = false,
                  const char* record_shm = nullptr,
                  const char* relay_recorder_socket = nullptr);
