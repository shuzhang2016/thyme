#pragma once

// fuzz_harness.h — shared deterministic fuzz driver kit (TST-03).
//
// No libFuzzer/clang dependency on the target toolchain (GCC 11), so the
// fuzz drivers are plain main() loops: replay a seed corpus directory, then
// run N deterministic pseudo-random STRUCTURED mutations (splitmix64, fixed
// seed) and assert the target parser never crashes, never hangs (bounded
// input), and always returns a defined result. Each driver keeps a
// THYME_FUZZ_LIBFUZZER entry point so the same RunOneInput() can later be
// wired to a real libFuzzer build.
//
// Driver shape (per target):
//   static void RunOneInput(const uint8_t* data, size_t size);  // no crash
//   static std::vector<uint8_t> Mutate(std::vector<uint8_t> in,
//                                      thyme_fuzz::SplitMix64* rng);
//   int main(int argc, char** argv) -> replay corpus + N mutations.
//
// Iterations/seed come from argv ([iterations] [seed]) or the environment
// (THYME_FUZZ_ITERS / THYME_FUZZ_SEED override), so the mr_gate quick pass
// and the nightly full pass share one binary.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>

namespace thyme_fuzz {

// splitmix64: tiny, portable, fully deterministic across platforms.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) : s_(seed) {}
  uint64_t Next() {
    uint64_t z = (s_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  // Uniform-ish value in [0, n); n == 0 yields 0 (callers guard emptiness).
  uint64_t Below(uint64_t n) { return n == 0 ? 0 : Next() % n; }
  bool Coin(uint32_t denominator) { return Below(denominator) == 0; }
  uint8_t Byte() { return static_cast<uint8_t>(Next() & 0xFFu); }
  void Fill(uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) p[i] = Byte();
  }
  template <typename T>
  const T& Pick(const std::vector<T>& v) {
    return v[Below(v.size())];
  }

 private:
  uint64_t s_;
};

// Mixes the run seed with the iteration ordinal so every iteration gets an
// independent, reproducible stream (same seed -> same mutations).
inline uint64_t IterSeed(uint64_t seed, uint64_t iter) {
  SplitMix64 m(seed ^ 0x7465737466797A7Aull);
  m.Next();
  return m.Next() ^ SplitMix64(iter).Next();
}

struct RunConfig {
  uint64_t iterations = 0;
  uint64_t seed = 0;
};

inline uint64_t EnvU64(const char* name, uint64_t fallback) {
  const char* v = std::getenv(name);
  if (v == nullptr || v[0] == '\0') return fallback;
  return std::strtoull(v, nullptr, 10);
}

inline RunConfig ParseRunConfig(int argc, char** argv, uint64_t default_iters,
                                uint64_t default_seed) {
  RunConfig cfg;
  cfg.iterations = default_iters;
  cfg.seed = default_seed;
  if (argc > 1) cfg.iterations = std::strtoull(argv[1], nullptr, 10);
  if (argc > 2) cfg.seed = std::strtoull(argv[2], nullptr, 10);
  cfg.iterations = EnvU64("THYME_FUZZ_ITERS", cfg.iterations);
  cfg.seed = EnvU64("THYME_FUZZ_SEED", cfg.seed);
  return cfg;
}

// Loads every regular file in `dir` (sorted by name, so corpus replay and
// mutation-base selection are deterministic across filesystems). Missing
// directory is not an error: in-code canonical seeds still apply.
inline std::vector<std::vector<uint8_t>> LoadCorpusDir(const char* dir) {
  std::vector<std::vector<uint8_t>> out;
  DIR* d = opendir(dir);
  if (d == nullptr) return out;
  std::vector<std::string> names;
  for (dirent* e = readdir(d); e != nullptr; e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    names.emplace_back(e->d_name);
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  for (const std::string& name : names) {
    const std::string path = std::string(dir) + "/" + name;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) continue;
    std::vector<uint8_t> bytes;
    uint8_t buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      bytes.insert(bytes.end(), buf, buf + n);
    }
    std::fclose(f);
    out.push_back(std::move(bytes));
  }
  return out;
}

// ---- Generic byte-level mutators ------------------------------------------
// All are bounds-safe; the result never exceeds `max_size` bytes.

inline void FlipRandomBit(std::vector<uint8_t>* b, SplitMix64* rng) {
  if (b->empty()) return;
  (*b)[rng->Below(b->size())] ^= static_cast<uint8_t>(1u << rng->Below(8));
}

inline void FlipRandomBytes(std::vector<uint8_t>* b, SplitMix64* rng,
                            std::size_t count) {
  for (std::size_t i = 0; i < count && !b->empty(); ++i) {
    (*b)[rng->Below(b->size())] ^= rng->Byte();
  }
}

inline void FillRandomSpan(std::vector<uint8_t>* b, SplitMix64* rng,
                           bool zero) {
  if (b->empty()) return;
  const std::size_t at = rng->Below(b->size());
  const std::size_t len = 1 + rng->Below(std::min<std::size_t>(
                                 b->size() - at, 32));
  for (std::size_t i = at; i < at + len; ++i) {
    (*b)[i] = zero ? 0 : rng->Byte();
  }
}

inline void Truncate(std::vector<uint8_t>* b, SplitMix64* rng) {
  if (b->empty()) return;
  b->resize(rng->Below(b->size()));  // may empty the input entirely
}

inline void ExtendRandom(std::vector<uint8_t>* b, SplitMix64* rng,
                         std::size_t max_size) {
  if (b->size() >= max_size) return;
  const std::size_t extra =
      1 + rng->Below(std::min<std::size_t>(max_size - b->size(), 64));
  const std::size_t old = b->size();
  b->resize(old + extra);
  rng->Fill(b->data() + old, extra);
}

inline void DeleteSpan(std::vector<uint8_t>* b, SplitMix64* rng) {
  if (b->size() < 2) return;
  const std::size_t at = rng->Below(b->size());
  const std::size_t len =
      1 + rng->Below(std::min<std::size_t>(b->size() - at, 32));
  b->erase(b->begin() + static_cast<std::ptrdiff_t>(at),
           b->begin() + static_cast<std::ptrdiff_t>(at + len));
}

inline void DuplicateSpan(std::vector<uint8_t>* b, SplitMix64* rng,
                          std::size_t max_size) {
  if (b->empty() || b->size() >= max_size) return;
  const std::size_t at = rng->Below(b->size());
  const std::size_t len =
      1 + rng->Below(std::min<std::size_t>(b->size() - at, 32));
  const std::size_t ins = rng->Below(b->size());
  std::vector<uint8_t> span(b->begin() + static_cast<std::ptrdiff_t>(at),
                            b->begin() + static_cast<std::ptrdiff_t>(at + len));
  if (b->size() + span.size() > max_size) return;
  b->insert(b->begin() + static_cast<std::ptrdiff_t>(ins), span.begin(),
            span.end());
}

inline void SwapSpans(std::vector<uint8_t>* b, SplitMix64* rng) {
  if (b->size() < 8) return;
  const std::size_t len = 1 + rng->Below(b->size() / 4);
  const std::size_t a = rng->Below(b->size() - len + 1);
  const std::size_t c = rng->Below(b->size() - len + 1);
  for (std::size_t i = 0; i < len; ++i) std::swap((*b)[a + i], (*b)[c + i]);
}

// Little-endian scalar poke at a random offset (simulates corrupted length /
// count / magic fields without knowing the format).
inline void PokeLe(std::vector<uint8_t>* b, SplitMix64* rng, int width,
                   uint64_t value) {
  if (b->size() < static_cast<std::size_t>(width)) return;
  const std::size_t at = rng->Below(b->size() - width + 1);
  for (int i = 0; i < width; ++i) {
    (*b)[at + i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

}  // namespace thyme_fuzz
