#include "thyme/record/triggered_recorder.h"

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace thyme {
namespace triggered {

namespace {

// Kind prefixes in a channel selector. Derived from the literal so a rename
// cannot leave a stale length behind.
constexpr char kPinnedSlotPrefix[] = "pinned_slot:";
constexpr std::size_t kPinnedSlotPrefixLen = sizeof(kPinnedSlotPrefix) - 1;

// TRG-16 selector glob: '*' matches any run of characters (including '/'),
// everything else is literal. Iterative two-pointer backtracking: no
// allocation, bounded by len(pattern) * len(text).
bool GlobMatch(const char* pattern, const char* text) {
  const char* star = nullptr;
  const char* retry = nullptr;
  while (*text != '\0') {
    if (*pattern == '*') {
      star = pattern++;
      retry = text;
    } else if (*pattern == *text) {
      ++pattern;
      ++text;
    } else if (star != nullptr) {
      pattern = star + 1;
      text = ++retry;
    } else {
      return false;
    }
  }
  while (*pattern == '*') ++pattern;
  return *pattern == '\0';
}

bool SelectorHits(const ChannelSelectorRule& rule, const std::string& name,
                  const std::string& shm) {
  if (GlobMatch(rule.selector.c_str(), name.c_str())) return true;
  if (rule.selector != name && GlobMatch(rule.selector.c_str(), shm.c_str())) {
    return true;
  }
  return false;
}

}  // namespace

std::string ChannelSourceName(const SourceConfig& source) {
  // A channel may be addressed by its configured `name` (frames/<name> for
  // PinnedSlot channels) or by its shm name. An aggregated mirror is also
  // addressable per record_channels entry (SelectRecordStruct).
  if (!source.channel_name.empty()) return source.channel_name;
  return source.shm_name;
}

bool ParseChannelSelectors(const std::vector<std::string>& entries,
                           std::vector<ChannelSelectorRule>* out,
                           std::string* err) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  for (const std::string& raw : entries) {
    std::string e = raw;
    ChannelSelectorRule rule;
    if (!e.empty() && e[0] == '!') {
      rule.exclude = true;
      e = e.substr(1);
    }
    const std::size_t at = e.find('@');
    if (at != std::string::npos) {
      const std::string dec = e.substr(at + 1);
      e = e.substr(0, at);
      if (dec.compare(0, 4, "dec=") != 0 || dec.size() == 4) {
        if (err != nullptr) {
          *err = "bad decimate suffix @" + dec;
        }
        return false;
      }
      char* end = nullptr;
      errno = 0;
      const long n = std::strtol(dec.c_str() + 4, &end, 10);
      if (errno != 0 || end == nullptr || *end != '\0' || n < 1 ||
          n > 1000000) {
        if (err != nullptr) {
          *err = "decimate out of range: " + dec;
        }
        return false;
      }
      rule.decimate = static_cast<uint32_t>(n);
    }
    if (e.compare(0, 13, "small_struct:") == 0 && e.size() > 13) {
      rule.has_kind = true;
      rule.pinned_slot = false;
      rule.selector = e.substr(13);
    } else if (e.compare(0, kPinnedSlotPrefixLen, kPinnedSlotPrefix) == 0 &&
               e.size() > kPinnedSlotPrefixLen) {
      rule.has_kind = true;
      rule.pinned_slot = true;
      rule.selector = e.substr(kPinnedSlotPrefixLen);
    } else if (!e.empty()) {
      rule.selector = e;
    } else {
      if (err != nullptr) {
        *err = "bad channel rule: " + raw;
      }
      return false;
    }
    if (rule.exclude && rule.decimate != 1) {
      if (err != nullptr) {
        *err = "decimate on exclude rule: " + raw;
      }
      return false;
    }
    out->push_back(rule);
  }
  return true;
}

const ChannelSelectorRule* MatchChannelSelector(
    const std::vector<ChannelSelectorRule>& rules,
    const SourceConfig& source) {
  const bool is_pinned_slot = source.kind == SourceConfig::Kind::kPinnedSlot;
  const std::string name = ChannelSourceName(source);
  const ChannelSelectorRule* match = nullptr;
  for (const ChannelSelectorRule& rule : rules) {
    if (rule.has_kind && rule.pinned_slot != is_pinned_slot) {
      continue;
    }
    const bool source_hit = SelectorHits(rule, name, source.shm_name);
    bool member_hit = false;
    for (const std::string& member : source.record_channels) {
      if (SelectorHits(rule, member, member)) member_hit = true;
    }
    if (!source_hit && !member_hit) continue;
    // A member exclude does not drop the rest of the aggregate. An exclude
    // that names the source itself still rejects the whole source.
    if (rule.exclude) {
      if (source_hit) return nullptr;
      continue;
    }
    if (match == nullptr || rule.selector != "*") match = &rule;
  }
  return match;
}

bool SelectRecordStruct(const std::vector<ChannelSelectorRule>& rules,
                        const SourceConfig& source, uint16_t sid,
                        uint32_t* decimate) {
  if (rules.empty() || source.record_channels.empty()) return true;
  if (sid >= source.record_channels.size()) return false;
  const bool is_pinned_slot = source.kind == SourceConfig::Kind::kPinnedSlot;
  const std::string& member = source.record_channels[sid];
  const std::string source_name = ChannelSourceName(source);
  const ChannelSelectorRule* match = nullptr;
  bool excluded = false;
  for (const ChannelSelectorRule& rule : rules) {
    if (rule.has_kind && rule.pinned_slot != is_pinned_slot) continue;
    const bool member_hit = SelectorHits(rule, member, member);
    const bool source_hit = SelectorHits(rule, source_name, source.shm_name);
    if (!member_hit && !source_hit) continue;
    if (rule.exclude) {
      if (member_hit) excluded = true;
      continue;
    }
    if (match == nullptr || rule.selector != "*") match = &rule;
  }
  if (excluded || match == nullptr) return false;
  if (decimate != nullptr) *decimate = match->decimate;
  return true;
}

bool ChannelSelectorMatches(const std::string& selector,
                            const SourceConfig& source) {
  std::vector<ChannelSelectorRule> rules;
  if (!ParseChannelSelectors({selector}, &rules, nullptr)) {
    return false;
  }
  return MatchChannelSelector(rules, source) != nullptr;
}

SourceConfig::CrcPolicy EffectiveCrcPolicy(const SourceConfig& source) {
  if (source.crc != SourceConfig::CrcPolicy::kDefault) return source.crc;
  // Defaults: bulk channels (point cloud/image/H264) record integrity at the
  // package level; small structs/control keep per-frame CRC.
  return source.kind == SourceConfig::Kind::kPinnedSlot
             ? SourceConfig::CrcPolicy::kPackage
             : SourceConfig::CrcPolicy::kFrame;
}

const char* CrcPolicyName(SourceConfig::CrcPolicy policy) {
  switch (policy) {
    case SourceConfig::CrcPolicy::kPackage: return "package";
    case SourceConfig::CrcPolicy::kFrame: return "frame";
    case SourceConfig::CrcPolicy::kOff: return "off";
    case SourceConfig::CrcPolicy::kDefault: break;
  }
  return "default";
}

bool ParseCrcPolicy(const std::string& text, SourceConfig::CrcPolicy* out) {
  if (out == nullptr) return false;
  if (text == "package") {
    *out = SourceConfig::CrcPolicy::kPackage;
  } else if (text == "frame") {
    *out = SourceConfig::CrcPolicy::kFrame;
  } else if (text == "off") {
    *out = SourceConfig::CrcPolicy::kOff;
  } else {
    return false;
  }
  return true;
}

integrity::ManifestMode EffectiveManifestMode(const SourceConfig& source,
                                              integrity::ManifestMode default_mode) {
  switch (source.manifest) {
    case SourceConfig::ManifestPolicy::kDefault: return default_mode;
    case SourceConfig::ManifestPolicy::kFull: return integrity::ManifestMode::kFull;
    case SourceConfig::ManifestPolicy::kMetadata: return integrity::ManifestMode::kMetadata;
    case SourceConfig::ManifestPolicy::kOff: return integrity::ManifestMode::kOff;
  }
  return default_mode;
}

const char* ManifestPolicyName(SourceConfig::ManifestPolicy policy) {
  switch (policy) {
    case SourceConfig::ManifestPolicy::kFull: return "full";
    case SourceConfig::ManifestPolicy::kMetadata: return "metadata";
    case SourceConfig::ManifestPolicy::kOff: return "off";
    case SourceConfig::ManifestPolicy::kDefault: break;
  }
  return "default";
}

bool ParseManifestPolicy(const std::string& text, SourceConfig::ManifestPolicy* out) {
  if (out == nullptr) return false;
  if (text == "full") {
    *out = SourceConfig::ManifestPolicy::kFull;
  } else if (text == "metadata") {
    *out = SourceConfig::ManifestPolicy::kMetadata;
  } else if (text == "off") {
    *out = SourceConfig::ManifestPolicy::kOff;
  } else {
    return false;
  }
  return true;
}

uint32_t TriggeredRecorder::DescriptorRingCapacity(
    uint64_t pending_bytes_quota) {
  // One descriptor per KiB of byte-quota budget (a SwapDescriptor is 40 B, so
  // the ring costs ~4% of the quota it guards). Clamped to keep tiny test
  // quotas workable and to cap worst-case memory at 2 * 64 KiB descriptors.
  constexpr uint32_t kMinCapacity = 256;
  constexpr uint32_t kMaxCapacity = 1u << 16;
  constexpr uint64_t kBytesPerDescriptor = 1024;
  uint64_t n = pending_bytes_quota / kBytesPerDescriptor;
  if (n < kMinCapacity) n = kMinCapacity;
  if (n > kMaxCapacity) n = kMaxCapacity;
  uint32_t cap = kMinCapacity;  // power of two
  while (cap < n) cap <<= 1;
  return cap;
}

}  // namespace triggered
}  // namespace thyme
