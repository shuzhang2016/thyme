// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/record_registry.h"
#include "support/pipeline_support.h"

inline void RecordTapC0(thyme::RecordMirrorRingPage* page, const thyme::Frame<Reading>& v) {
  RecordTapMirror(page, RecordStructId::kC0, &v, sizeof(v), NowMonoNs(), v.hdr.sequence);
}

inline void RecordTapC1(thyme::RecordMirrorRingPage* page, const thyme::Frame<Reading>& v) {
  RecordTapMirror(page, RecordStructId::kC1, &v, sizeof(v), NowMonoNs(), v.hdr.sequence);
}

inline void RecordTapC2(thyme::RecordMirrorRingPage* page, const thyme::Frame<Reading>& v) {
  RecordTapMirror(page, RecordStructId::kC2, &v, sizeof(v), NowMonoNs(), v.hdr.sequence);
}

inline void RecordTapC3(thyme::RecordMirrorRingPage* page, const thyme::Frame<Reading>& v) {
  RecordTapMirror(page, RecordStructId::kC3, &v, sizeof(v), NowMonoNs(), v.hdr.sequence);
}

