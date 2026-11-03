// Code generated for the ports fixture graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/record_registry.h"
#include "support/pipeline_support.h"

inline void RecordTapPayload(thyme::RecordMirrorRingPage* page, const thyme::Frame<Foo>& v) {
  RecordTapMirror(page, RecordStructId::kPayload, &v, sizeof(v), NowMonoNs(), v.hdr.sequence);
}

