// CRC32 hardware-acceleration cross-check and microbenchmark.
// Anchors the dispatching Crc32Update against the bitwise reference on random
// data (including a 240 KB frame payload) and prints seal-time speedup.
#include "thyme/check.h"
#include "thyme/contracts/frame_header.h"

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace thyme;

namespace {

uint32_t NextRand(uint32_t& s) {
  s = s * 1664525u + 1013904223u;
  return s;
}

std::vector<uint8_t> RandomBuffer(std::size_t n, uint32_t seed) {
  std::vector<uint8_t> buf(n);
  uint32_t s = seed;
  for (std::size_t i = 0; i < n; ++i) {
    buf[i] = static_cast<uint8_t>(NextRand(s) >> 24);
  }
  return buf;
}

void CrossCheck(const uint8_t* data, std::size_t n) {
  const uint32_t ref = detail::Crc32UpdateBitwise(0xFFFFFFFFu, data, n) ^ 0xFFFFFFFFu;
  CHECK(Crc32(data, n) == ref);
  // Incremental (segmented) calls must compose identically in both paths.
  for (std::size_t cut = 0; cut <= n && cut <= 40; cut += 7) {
    uint32_t a = detail::Crc32UpdateBitwise(0xFFFFFFFFu, data, cut);
    a = detail::Crc32UpdateBitwise(a, data + cut, n - cut);
    uint32_t b = Crc32Update(0xFFFFFFFFu, data, cut);
    b = Crc32Update(b, data + cut, n - cut);
    CHECK(a == b);
    CHECK((b ^ 0xFFFFFFFFu) == ref);
  }
}

// Software model of the ACLE path (Crc32UpdateArm): each __crc32b/h/w/d
// "instruction" is emulated by stepping the reflected CRC-32 over the chunk's
// bytes, LSB first (little-endian). Pure software — runs on any host and
// permanently pins the wrapping algebra: segmented 8/4/2/1 instruction
// composition must equal the whole-buffer CRC with the raw (un-inverted)
// running accumulator.
uint32_t ArmInsn(uint32_t x, uint64_t v, std::size_t width) {
  for (std::size_t i = 0; i < width; ++i) {
    const uint8_t b = static_cast<uint8_t>(v >> (8 * i));
    x = detail::Crc32UpdateBitwise(x, &b, 1);
  }
  return x;
}

uint32_t Crc32UpdateArmModel(uint32_t c, const uint8_t* data, std::size_t n) {
  uint32_t x = c;
  while (n >= sizeof(uint64_t)) {
    uint64_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = ArmInsn(x, v, sizeof(v));
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n >= sizeof(uint32_t)) {
    uint32_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = ArmInsn(x, v, sizeof(v));
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n >= sizeof(uint16_t)) {
    uint16_t v = 0;
    std::memcpy(&v, data, sizeof(v));
    x = ArmInsn(x, v, sizeof(v));
    data += sizeof(v);
    n -= sizeof(v);
  }
  if (n != 0) {
    x = ArmInsn(x, *data, 1);
  }
  return x;
}

void ModelCheck(const uint8_t* data, std::size_t n) {
  const uint32_t whole = detail::Crc32UpdateBitwise(0xFFFFFFFFu, data, n);
  CHECK(Crc32UpdateArmModel(0xFFFFFFFFu, data, n) == whole);
  // Segmented model calls must compose, and the caller-side final XOR must
  // recover the wire CRC — i.e. the raw accumulator convention is correct.
  for (std::size_t cut = 0; cut <= n && cut <= 40; cut += 7) {
    uint32_t m = Crc32UpdateArmModel(0xFFFFFFFFu, data, cut);
    m = Crc32UpdateArmModel(m, data + cut, n - cut);
    CHECK(m == whole);
    CHECK((m ^ 0xFFFFFFFFu) == Crc32(data, n));
  }
}

using Clock = std::chrono::steady_clock;

uint64_t TimeNs(uint32_t (*fn)(uint32_t, const uint8_t*, std::size_t),
                const uint8_t* data, std::size_t n, int iters) {
  uint32_t c = 0xFFFFFFFFu;
  const auto t0 = Clock::now();
  for (int i = 0; i < iters; ++i) {
    c = fn(c, data, n);
  }
  const auto t1 = Clock::now();
  CHECK(c != 0);  // keep the loop alive
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

}  // namespace

int main() {
  // Classic CRC-32 check value anchors the wire semantics.
  CHECK(Crc32(reinterpret_cast<const uint8_t*>("123456789"), 9) == 0xCBF43926u);

  // Exhaustive small sizes + segmented boundaries around the 16-byte fold.
  for (std::size_t n = 0; n <= 130; ++n) {
    const std::vector<uint8_t> buf = RandomBuffer(n, 0xDEAD0000u + static_cast<uint32_t>(n));
    CrossCheck(buf.data(), n);
  }
  // Large frames: 240 KB payload and a few odd sizes above it.
  for (std::size_t n :
       {std::size_t{1024}, std::size_t{4096}, std::size_t{65535},
        std::size_t{240} * 1024, std::size_t{240} * 1024 + 13}) {
    const std::vector<uint8_t> buf = RandomBuffer(n, 0x240000u);
    CrossCheck(buf.data(), n);
  }

  // ACLE-path software model: exhaustive small sizes, large sizes, fuzz.
  for (std::size_t n = 0; n <= 130; ++n) {
    const std::vector<uint8_t> buf = RandomBuffer(n, 0xAC1E0000u + static_cast<uint32_t>(n));
    ModelCheck(buf.data(), n);
  }
  for (std::size_t n :
       {std::size_t{1024}, std::size_t{4096}, std::size_t{65535},
        std::size_t{240} * 1024, std::size_t{240} * 1024 + 13}) {
    const std::vector<uint8_t> buf = RandomBuffer(n, 0xACE000u);
    ModelCheck(buf.data(), n);
  }
  {
    uint32_t s = 0xF00Du;
    for (int i = 0; i < 2000; ++i) {
      const std::size_t n = NextRand(s) % 300;
      const std::vector<uint8_t> buf = RandomBuffer(n, s);
      ModelCheck(buf.data(), n);
    }
  }

#if defined(THYME_CRC32_X86_PCLMUL)
  CHECK(detail::Crc32HwAccelerated());  // lab host must exercise the HW path
#endif

  // Microbenchmark: 240 KB frame CRC, dispatched vs bitwise reference.
  const std::vector<uint8_t> frame = RandomBuffer(std::size_t{240} * 1024, 0xC0FFEEu);
  constexpr int kIters = 50;
  const uint64_t hw_ns = TimeNs(Crc32Update, frame.data(), frame.size(), kIters);
  const uint64_t ref_ns =
      TimeNs(detail::Crc32UpdateBitwise, frame.data(), frame.size(), kIters);
  const double hw_us = static_cast<double>(hw_ns) / (1000.0 * kIters);
  const double ref_us = static_cast<double>(ref_ns) / (1000.0 * kIters);
  std::printf("crc32 240KB seal: dispatched=%.1f us bitwise=%.1f us speedup=%.1fx "
              "(hw_accel=%d)\n",
              hw_us, ref_us, ref_us / hw_us,
              static_cast<int>(detail::Crc32HwAccelerated()));

  return 0;
}
