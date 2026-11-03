#include "abi14/contracts/types.h"

#include <cstdint>

namespace {

template <typename FrameT, typename LengthT>
bool SealedFrameOk(LengthT n) {
  FrameT msg;
  thyme::FrameInit(msg, 7u, 11u, 13u);
  msg.payload.n = n;
  thyme::FrameSeal(msg, kFrameMagic, kFrameVersion);
  return thyme::FrameVerify(msg, kFrameMagic, kFrameVersion);
}

template <typename FrameT, typename LengthT>
bool SignedBoundariesOk() {
  if (!SealedFrameOk<FrameT>(LengthT{0})) {
    return false;
  }
  if (!SealedFrameOk<FrameT>(LengthT{4})) {
    return false;
  }
#ifndef NDEBUG
  return !SealedFrameOk<FrameT>(LengthT{-1}) &&
         !SealedFrameOk<FrameT>(LengthT{5});
#else
  return SealedFrameOk<FrameT>(LengthT{-1}) &&
         SealedFrameOk<FrameT>(LengthT{5});
#endif
}

}  // namespace

int main() {
  if (!SealedFrameOk<thyme::Frame<Packet_t>>(4u)) {
    return 1;
  }
  if (!SealedFrameOk<thyme::Frame<Packet_t>>(0u)) {
    return 2;
  }
#ifndef NDEBUG
  if (SealedFrameOk<thyme::Frame<Packet_t>>(5u)) {
    return 3;
  }
#else
  if (!SealedFrameOk<thyme::Frame<Packet_t>>(5u)) {
    return 4;
  }
#endif
  if (!SignedBoundariesOk<thyme::Frame<Signed32Packet_t>, int32_t>()) {
    return 5;
  }
  if (!SignedBoundariesOk<thyme::Frame<Signed64Packet_t>, int64_t>()) {
    return 6;
  }
  return 0;
}
