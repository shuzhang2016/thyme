#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "thyme/contracts/frame_header.h"

namespace thyme {
namespace record_control {

inline constexpr uint32_t kControlMagic = 0x4c544352u;  // "RCTL" (LE)
inline constexpr uint16_t kControlVersionLegacy = 1;
inline constexpr uint16_t kControlVersion = 2;
inline constexpr std::size_t kMaxControlPayload = 4096;

// The text commands used before SEC-10 are protocol V1. V2 is deliberately
// independent of trigger_proto: record_control keeps its own command namespace.
enum class ControlOp : uint16_t {
  kTrigger = 1,
  kArm = 2,
  kDisarm = 3,
  kStatus = 4,
  kStatusJson = 5,
};

enum class ControlCode : uint16_t {
  kAccepted = 0,
  kUnsupportedVersion = 1,
  kReplayRejected = 2,
  kAuthRejected = 3,
  kMalformedRequest = 4,
};

struct ControlRequest {
  uint32_t magic = kControlMagic;
  uint16_t version = kControlVersion;
  uint16_t op = 0;
  uint64_t sequence = 0;
  uint32_t crc32 = 0;
  uint32_t reserved0 = 0;
};

struct ControlResponse {
  uint32_t magic = kControlMagic;
  uint16_t version = kControlVersion;
  uint16_t code = 0;
  uint64_t sequence = 0;
  uint32_t payload_size = 0;
  uint32_t crc32 = 0;
};

inline uint32_t RequestCrc(const ControlRequest &request) {
  return Crc32(reinterpret_cast<const uint8_t *>(&request),
               offsetof(ControlRequest, crc32));
}

inline void StampRequestCrc(ControlRequest *request) {
  request->crc32 = RequestCrc(*request);
}

inline uint32_t ResponseCrc(const ControlResponse &response,
                            const char *payload = nullptr) {
  uint32_t crc =
      Crc32Update(0xFFFFFFFFu, reinterpret_cast<const uint8_t *>(&response),
                  offsetof(ControlResponse, crc32));
  if (response.payload_size != 0 && payload != nullptr) {
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t *>(payload),
                      response.payload_size);
  }
  return crc ^ 0xFFFFFFFFu;
}

inline void StampResponseCrc(ControlResponse *response,
                             const char *payload = nullptr) {
  response->crc32 = ResponseCrc(*response, payload);
}

static_assert(sizeof(ControlRequest) == 24, "ControlRequest wire size");
static_assert(offsetof(ControlRequest, crc32) == 16,
              "ControlRequest CRC is the trailer");
static_assert(sizeof(ControlResponse) == 24, "ControlResponse wire size");
static_assert(offsetof(ControlResponse, crc32) == 20,
              "ControlResponse CRC is the trailer");
static_assert(std::is_standard_layout<ControlRequest>::value &&
                  std::is_trivially_copyable<ControlRequest>::value,
              "ControlRequest crosses processes");
static_assert(std::is_standard_layout<ControlResponse>::value &&
                  std::is_trivially_copyable<ControlResponse>::value,
              "ControlResponse crosses processes");

// TRG-15: optional tail of a V2 kTrigger datagram. The header stays 24 bytes
// so a bare trigger command is unchanged. Frame bytes are not in this tail;
// the recorder reads them from the shared-memory mirror or big-frame ring.
inline constexpr uint32_t kTriggerParamsMagic = 0x31504752u;  // "RGP1"
inline constexpr uint16_t kTriggerParamsVersion = 1;
inline constexpr uint16_t kTriggerParamFlagRealtime = 1u;
inline constexpr uint16_t kTriggerParamFlagHigh = 2u;
inline constexpr std::size_t kMaxTriggerChannels = 1024;

struct TriggerParams {
  uint32_t magic = kTriggerParamsMagic;
  uint16_t version = kTriggerParamsVersion;
  uint16_t flags = 0;
  uint64_t trigger_id = 0;
  uint64_t before_ns = 0;
  uint64_t after_ns = 0;
  uint64_t t0_mono_ns = 0;
  uint32_t channel_bytes = 0;
  uint32_t crc32 = 0;
};

inline uint32_t TriggerParamsCrc(const TriggerParams &params,
                                 const char *channels) {
  uint32_t crc = Crc32Update(0xFFFFFFFFu,
                             reinterpret_cast<const uint8_t *>(&params),
                             offsetof(TriggerParams, crc32));
  if (params.channel_bytes != 0 && channels != nullptr) {
    crc = Crc32Update(crc, reinterpret_cast<const uint8_t *>(channels),
                      params.channel_bytes);
  }
  return crc ^ 0xFFFFFFFFu;
}

inline void StampTriggerParams(TriggerParams *params, const char *channels) {
  params->crc32 = TriggerParamsCrc(*params, channels);
}

static_assert(sizeof(TriggerParams) == 48, "TriggerParams wire size");
static_assert(offsetof(TriggerParams, crc32) == 44,
              "TriggerParams CRC is the trailer");
static_assert(std::is_standard_layout<TriggerParams>::value &&
                  std::is_trivially_copyable<TriggerParams>::value,
              "TriggerParams crosses processes");

}  // namespace record_control
}  // namespace thyme
