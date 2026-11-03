#pragma once
#include <cstddef>

namespace thyme {
// Minimal C++17 span over a contiguous [data, data+size) range (std::span is
// C++20). Used to pass generated constexpr graph tables to thyme::app.
template <typename T>
class span {
 public:
  constexpr span() = default;
  constexpr span(T* data, std::size_t size) : data_(data), size_(size) {}
  template <std::size_t N>
  constexpr span(T (&arr)[N]) : data_(arr), size_(N) {}

  constexpr T* data() const { return data_; }
  constexpr std::size_t size() const { return size_; }
  constexpr bool empty() const { return size_ == 0; }
  constexpr T& operator[](std::size_t i) const { return data_[i]; }
  constexpr T* begin() const { return data_; }
  constexpr T* end() const { return data_ + size_; }

 private:
  T* data_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace thyme
