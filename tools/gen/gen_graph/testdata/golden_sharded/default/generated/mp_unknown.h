// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/graph_ir.h"
#include "thyme/runtime/cyclic_thread.h"
#include "thyme/runtime/event_thread.h"
#include "thyme/runtime/notif_pipe.h"
#include "thyme/runtime/health_slot.h"
#include "support/record_mirror_ctx.h"
#include "generated/mp_role_locals.h"
#include "nodes/sink__s0.h"
#include "nodes/sink__s1.h"


// Role-local event plumbing: one member per edge into an
// exec=event node, plus futex wake state for queue consumers.
// OpenEventShmChannels maps the shm rings/wakes; in-proc pipes
// need no Open. Factories take EventTransports& instead of a flat
// per-edge parameter list.
struct EventTransports {
  thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s0_c0_pipe;
  thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s1_c1_pipe;
  thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s0_c2_pipe;
  thyme::NotifPipe<thyme::Frame<Reading>, 8> sink__s1_c3_pipe;
};

inline void SetEventRecordTaps(EventTransports& l) {
  l.sink__s0_c0_pipe.SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::kC0));
  l.sink__s1_c1_pipe.SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::kC1));
  l.sink__s0_c2_pipe.SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::kC2));
  l.sink__s1_c3_pipe.SetRecordTap(record_mirror::Page(), static_cast<uint16_t>(RecordStructId::kC3));
}

inline void ClearEventRecordTaps(EventTransports& l) {
  l.sink__s0_c0_pipe.ClearRecordTap();
  l.sink__s1_c1_pipe.ClearRecordTap();
  l.sink__s0_c2_pipe.ClearRecordTap();
  l.sink__s1_c3_pipe.ClearRecordTap();
}

// Constructs the handwritten thread for an unknown node type, using
// RoleLocals the role code assembles before dispatch.
// Returns nullptr for indices that are not unknown nodes (or if the
// graph has no unknown nodes); RunRole then stderr+exit 2.
inline thyme::CyclicThread* MakeUnknownThread(
    std::size_t node_index,
    RoleLocals& l,
    EventTransports& ev,
    thyme::HealthSlot* health_slot) {
  (void)node_index;
  (void)l;
  (void)ev;
  (void)health_slot;
  return nullptr;
}

// Constructs the handwritten thread for an exec=event node, with its
// in-edge pipes registered as wait inputs. Queue consumers also get
// SetWakeWord on the shared _wk_<node> word (mandatory; roles must
// not hand-wire). Returns nullptr for indices that are not event
// nodes.
inline thyme::EventThread* MakeEventThread(
    std::size_t node_index,
    EventTransports& ev,
    RoleLocals& l,
    thyme::HealthSlot* health_slot) {
  (void)node_index;
  (void)ev;
  (void)l;
  (void)health_slot;
  if (node_index == 1) {
    auto* t = new SinkS0Thread(SinkS0Ports{&ev.sink__s0_c0_pipe, &ev.sink__s0_c2_pipe}, MpContext{health_slot});
    thyme::Waitable* ws[] = {&ev.sink__s0_c0_pipe, &ev.sink__s0_c2_pipe};
    t->SetWaitInputs(ws, sizeof(ws) / sizeof(ws[0]));
  {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 40;
    (*t).SetPlacement(p);
  }
    return t;
  }
  if (node_index == 2) {
    auto* t = new SinkS1Thread(SinkS1Ports{&ev.sink__s1_c1_pipe, &ev.sink__s1_c3_pipe}, MpContext{health_slot});
    thyme::Waitable* ws[] = {&ev.sink__s1_c1_pipe, &ev.sink__s1_c3_pipe};
    t->SetWaitInputs(ws, sizeof(ws) / sizeof(ws[0]));
  {
    thyme::ThreadPlacement p{};
    p.ncpus = 1;
    p.cpus[0] = 0;
    p.priority = 40;
    (*t).SetPlacement(p);
  }
    return t;
  }
  return nullptr;
}
