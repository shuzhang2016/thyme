// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "thyme/app/mp_runtime.h"
#include "thyme/app/mp_control.h"
#include <memory>
#include "generated/graph_ir.h"
#include "generated/mp_ports.h"

// Per-role pools, rings, SHM latest adapters, and catalog extras.
// AssembleRoleLocals wires cross-process pipes before Make*Thread.
struct RoleLocals {

  static std::unique_ptr<RoleLocals> Create();


 private:
  RoleLocals() {}
};

inline std::unique_ptr<RoleLocals> RoleLocals::Create() {
  return std::unique_ptr<RoleLocals>(new RoleLocals());
}

inline bool AssembleRoleLocals(RoleLocals& l, const char* role,
                                 thyme::app::MpRoleChannels& channels,
                                 thyme::app::MpControl* ctrl) {
  (void)l;
  (void)role;
  (void)channels;
  (void)ctrl;
  return true;
}
