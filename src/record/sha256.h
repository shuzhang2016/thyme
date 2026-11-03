#pragma once

// sha256.h — self-contained SHA-256 and HMAC-SHA256 (FIPS 180-4 / RFC 2104).
//
// Internal to src/record: the tree has no crypto dependency and SEC-03 must
// not add one. One-shot helpers only (no streaming API is exposed): the
// evidence manifest hashes files in 64 KiB reads through the context below
// and signs/verifies the (small) manifest buffer.
//
// This is a plain C implementation: no heap, no exceptions, constant
// structure. It is *not* hardened against side channels beyond the usual
// table-free HMAC construction; SEC-03 signing is an integrity/attribution
// control, not a secret-key confidentiality control.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace thyme {
namespace integrity {
namespace sha256_detail {

inline uint32_t Rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

struct Ctx {
  uint32_t h[8];
  uint64_t total;
  uint8_t buf[64];
  uint32_t buflen;
};

inline const uint32_t* K() {
  static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
  return k;
}

inline void Init(Ctx* c) {
  c->h[0] = 0x6a09e667u;
  c->h[1] = 0xbb67ae85u;
  c->h[2] = 0x3c6ef372u;
  c->h[3] = 0xa54ff53au;
  c->h[4] = 0x510e527fu;
  c->h[5] = 0x9b05688cu;
  c->h[6] = 0x1f83d9abu;
  c->h[7] = 0x5be0cd19u;
  c->total = 0;
  c->buflen = 0;
}

inline void Block(uint32_t* h, const uint8_t* p) {
  uint32_t w[64];
  for (uint32_t i = 0; i < 16; ++i) {
    const std::size_t offset = std::size_t{4} * i;
    w[i] = (static_cast<uint32_t>(p[offset]) << 24) |
           (static_cast<uint32_t>(p[offset + 1]) << 16) |
           (static_cast<uint32_t>(p[offset + 2]) << 8) |
           static_cast<uint32_t>(p[offset + 3]);
  }
  for (uint32_t i = 16; i < 64; ++i) {
    const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^
                        (w[i - 15] >> 3);
    const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^
                        (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0];
  uint32_t b = h[1];
  uint32_t c = h[2];
  uint32_t d = h[3];
  uint32_t e = h[4];
  uint32_t f = h[5];
  uint32_t g = h[6];
  uint32_t hh = h[7];
  const uint32_t* k = K();
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
    const uint32_t ch = (e & f) ^ ((~e) & g);
    const uint32_t t1 = hh + s1 + ch + k[i] + w[i];
    const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = s0 + maj;
    hh = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  h[0] += a;
  h[1] += b;
  h[2] += c;
  h[3] += d;
  h[4] += e;
  h[5] += f;
  h[6] += g;
  h[7] += hh;
}

inline void Update(Ctx* c, const void* data, std::size_t n) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  c->total += n;
  if (c->buflen != 0) {
    const std::size_t take = (64 - c->buflen < n) ? (64 - c->buflen) : n;
    std::memcpy(c->buf + c->buflen, p, take);
    c->buflen += static_cast<uint32_t>(take);
    p += take;
    n -= take;
    if (c->buflen == 64) {
      Block(c->h, c->buf);
      c->buflen = 0;
    }
  }
  while (n >= 64) {
    Block(c->h, p);
    p += 64;
    n -= 64;
  }
  if (n != 0) {
    std::memcpy(c->buf, p, n);
    c->buflen = static_cast<uint32_t>(n);
  }
}

inline void Final(Ctx* c, uint8_t out[32]) {
  const uint64_t bits = c->total * 8;
  const uint8_t one = 0x80;
  Update(c, &one, 1);
  const uint8_t zero = 0;
  while (c->buflen != 56) Update(c, &zero, 1);
  uint8_t len[8];
  for (uint32_t i = 0; i < 8; ++i) {
    len[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
  }
  Update(c, len, 8);
  for (uint32_t i = 0; i < 8; ++i) {
    const std::size_t offset = std::size_t{4} * i;
    out[offset] = static_cast<uint8_t>(c->h[i] >> 24);
    out[offset + 1] = static_cast<uint8_t>(c->h[i] >> 16);
    out[offset + 2] = static_cast<uint8_t>(c->h[i] >> 8);
    out[offset + 3] = static_cast<uint8_t>(c->h[i]);
  }
}

}  // namespace sha256_detail

inline void Sha256(const void* data, std::size_t n, uint8_t out[32]) {
  sha256_detail::Ctx c{};
  sha256_detail::Init(&c);
  sha256_detail::Update(&c, data, n);
  sha256_detail::Final(&c, out);
}

inline void HmacSha256(const uint8_t* key, std::size_t key_len,
                       const void* data, std::size_t data_len,
                       uint8_t out[32]) {
  uint8_t k[64];
  std::memset(k, 0, sizeof(k));
  if (key_len > sizeof(k)) {
    Sha256(key, key_len, k);  // first 32 bytes, rest stay zero
  } else {
    std::memcpy(k, key, key_len);
  }
  uint8_t ipad[64];
  uint8_t opad[64];
  for (std::size_t i = 0; i < sizeof(k); ++i) {
    ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36u);
    opad[i] = static_cast<uint8_t>(k[i] ^ 0x5cu);
  }
  uint8_t inner[32];
  sha256_detail::Ctx c{};
  sha256_detail::Init(&c);
  sha256_detail::Update(&c, ipad, sizeof(ipad));
  sha256_detail::Update(&c, data, data_len);
  sha256_detail::Final(&c, inner);
  sha256_detail::Init(&c);
  sha256_detail::Update(&c, opad, sizeof(opad));
  sha256_detail::Update(&c, inner, sizeof(inner));
  sha256_detail::Final(&c, out);
  std::memset(k, 0, sizeof(k));
  std::memset(ipad, 0, sizeof(ipad));
  std::memset(opad, 0, sizeof(opad));
}

inline std::string HexEncode(const uint8_t* data, std::size_t n) {
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.resize(n * 2);
  for (std::size_t i = 0; i < n; ++i) {
    out[2 * i] = kHex[data[i] >> 4];
    out[2 * i + 1] = kHex[data[i] & 0x0Fu];
  }
  return out;
}

}  // namespace integrity
}  // namespace thyme
