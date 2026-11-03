// Code generated for the generated graph. DO NOT EDIT.
// Hand logic belongs in src/nodes/.
// gen_graph 3.11.1 (output format 3)
#pragma once

#include "generated/record_registry.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include "thyme/contracts/frame_header.h"

namespace {
inline void VizB64(std::string& out, const uint8_t* p, std::size_t n) {
  static const char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  for (std::size_t i = 0; i < n; i += 3) {
    const uint32_t a = p[i];
    const uint32_t b = (i + 1 < n) ? p[i + 1] : 0;
    const uint32_t c = (i + 2 < n) ? p[i + 2] : 0;
    const uint32_t x = (a << 16) | (b << 8) | c;
    out += kTab[(x >> 18) & 63];
    out += kTab[(x >> 12) & 63];
    out += (i + 1 < n) ? kTab[(x >> 6) & 63] : '=';
    out += (i + 2 < n) ? kTab[x & 63] : '=';
  }
}
}  // namespace

inline const char* VizSchemaSlot(uint16_t sid) {
  (void)sid;
  return "{\"type\":\"object\",\"properties\":{\"mono_ns\":{\"type\":\"integer\"},\"source_sequence\":{\"type\":\"integer\"},\"payload_b64\":{\"type\":\"string\"}}}";
}

inline bool VizEncodeSlot(uint16_t sid, const uint8_t* bytes, std::size_t n, std::string* out) {
  if (out == nullptr || bytes == nullptr) return false;
  (void)sid;
  const thyme::FrameHeader* h = reinterpret_cast<const thyme::FrameHeader*>(bytes);
  out->append("{\"mono_ns\":");
  out->append(std::to_string(h->mono_ns));
  out->append(",\"source_sequence\":");
  out->append(std::to_string(h->sequence));
  out->append(",\"payload_b64\":\"");
  VizB64(*out, bytes, n);
  out->append("\"}");
  return true;
}
