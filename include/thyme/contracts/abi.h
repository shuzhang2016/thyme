#pragma once

#include <cstddef>
#include <cstdint>

namespace thyme {

// ABI-02: byte order of everything thyme shares between processes or writes
// to disk — FrameHeader / Frame<T> (contracts/frame_header.h), the trigger
// control socket (record/trigger_protocol.h), .thyme.bin, the evidence
// manifest, and every SHM layout (runtime/*.h). The fixed structs are written
// in HOST byte order (they are memcpy'd/sealed as-is, with no byte-swapping
// layer), so a big-endian build would silently misinterpret every multi-byte
// field and fail its CRC/length checks in confusing ways. Reject such a build
// at compile time instead of shipping conversions: no thyme target is
// big-endian. Byte-oriented encoders (perf_log, manifest) write LSB-first
// explicitly and are host-independent; the assert still applies because the
// struct-cast formats above share this translation unit's contract.
// See docs/reference/abi-compatibility.md § Byte order.
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
inline constexpr bool kAbiHostIsLittleEndian =
    __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__;
// True only when the compiler's byte-order macros proved the assertion above;
// the ABI test uses it to require the check on GCC/Clang.
inline constexpr bool kAbiHostEndianChecked = true;
static_assert(kAbiHostIsLittleEndian,
              "thyme wire/SHM ABI is little-endian: wire structs are written "
              "in host byte order (no swap layer), so a big-endian host "
              "cannot interoperate (docs/reference/abi-compatibility.md)");
#else
// Compilers without __BYTE_ORDER__ (not GCC/Clang): the documented wire
// layouts are little-endian; no compile-time proof is available here.
inline constexpr bool kAbiHostIsLittleEndian = true;
inline constexpr bool kAbiHostEndianChecked = false;
#endif

// Single source for ABI-relevant ceilings. Module headers alias these
// (kThymeBinMaxPayloadSize, kRecordMirrorMaxPayload, App::kMaxThreads,
// EventThread::kMaxWaitInputs, CyclicThread::kMaxWaitInputs, ...);
// thyme/limits.h pins the literal values with static_asserts.
// NOTE: this 512 KB is the RECORD-layer ceiling (mirror ring slots and the
// thyme_bin codec). Frame<T> transport is separately capped at 64 KB by the
// uint16_t hdr.size field — contracts above 64 KB ride PinnedSlotStore.
inline constexpr uint32_t kAbiMaxPayloadBytes = 524288;  // 512 KB
// GEN-08: transport ceiling for a framed graph edge (FrameHeader + payload
// wire bytes). hdr.size is uint16_t, so the largest representable wire image
// is 0xFFFF. Generated code asserts FramePodWireBytes<Frame<T>> against this
// per channel; gen_graph additionally rejects a schema-known contract above
// it at generation time with a `file:line` diagnostic.
inline constexpr std::size_t kAbiMaxTransportBytes = 0xFFFFu;  // 64 KB - 1
// ABI-13: build-time capacity profile. Two ceilings size fixed SHM tables —
// the record-mirror struct table and the health-page worker slots — so they
// are the only ABI numbers a build may override. Every page that embeds them
// mixes the value into ShmLayoutHash(), therefore a peer built with another
// profile is rejected at attach instead of being read with the wrong table
// size (bump-only, no dual-read; ARCH-11).
//
// The wire *format* ceilings stay fixed on purpose: kAbiMaxPayloadBytes and
// kRecordMirrorMaxArenaBytes are coupled to the .thyme.bin / mirror encoding,
// and kAbiMaxTransportBytes is the uint16_t hdr.size wire image.
//
// SPMC readers stay fixed at 8 in ABI-13: ShmSpmcRing places payload at
// kShmSpmcPayloadOffset = 320 (five 64 B header cells) and the eight
// per-reader drop slots fill the fifth cell exactly. Raising the ceiling needs
// that offset to move — a page-layout change of its own, not a profile knob.
// The macro exists so the constraint is greppable and a future change has one
// place to edit; limits.h asserts the only supported value.
//
// Override through CMake (THYME_ABI_MAX_STRUCT_COUNT / _THREADS), which
// defines these macros. thyme/limits.h re-pins the default literals unless
// THYME_ABI_PROFILE_CUSTOM is defined.
#ifndef THYME_ABI_MAX_SPMC_READERS
#define THYME_ABI_MAX_SPMC_READERS 8
#endif
#ifndef THYME_ABI_MAX_STRUCT_COUNT
#define THYME_ABI_MAX_STRUCT_COUNT 64
#endif
#ifndef THYME_ABI_MAX_THREADS
#define THYME_ABI_MAX_THREADS 64
#endif
inline constexpr uint32_t kAbiMaxStructCount = THYME_ABI_MAX_STRUCT_COUNT;
// Record-mirror payload arena budget: the aggregate of every channel's
// per-slot capacity (round_up(payload_size, 64)) must fit within this many
// bytes. Sized to cover the perception-heavy graph (summed contract sizes
// ~460 KB) with headroom, far below the 64 × 512 KB (= 32 MB) worst case a
// fixed per-slot ceiling would impose.
inline constexpr uint32_t kRecordMirrorMaxArenaBytes = 2u * 1024u * 1024u;
inline constexpr std::size_t kAbiMaxThreads = THYME_ABI_MAX_THREADS;
inline constexpr std::size_t kAbiMaxEventWaitInputs = 32;
inline constexpr std::size_t kAbiMaxCyclicWaitInputs = 8;
inline constexpr std::size_t kAbiMaxSpmcReaders = THYME_ABI_MAX_SPMC_READERS;

}  // namespace thyme
