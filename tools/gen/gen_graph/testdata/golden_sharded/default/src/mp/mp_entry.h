#pragma once

// Seam A: mp entry points shared between PLAN-2 and PLAN-3.
//
// PLAN-2 owns these declarations + throwaway stubs; PLAN-3 replaces the stubs
// with the real role/supervisor implementation while keeping these signatures
// byte-identical. Do not change them.

int RunRole(const char* prefix, const char* role, const char* record_shm,
            int proc_idx);
int RunSupervisor(const char* self_exe, int seconds, bool record,
                  const char* record_shm, const char* relay_recorder_socket);
