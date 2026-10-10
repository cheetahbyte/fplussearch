#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <string>
#include <string_view>

#include "storage/mapped_array.hpp"

namespace fplussearch {

struct DocIn {
  std::string_view path;  // into a PathArena
  uint64_t size;
  uint32_t mtime;
  uint64_t mtime_ns = 0, ctime_ns = 0;
};

namespace content_detail {

// Paths stored back to back: a million of them as separate strings would
// cost twice the memory, most of it allocator overhead. The buffer is
// reserved, never grown, so views into it stay valid (unwritten reserved
// pages cost nothing).
class PathArena {
 public:
  PathArena() { bytes_.reserve(kCapacity); }
  std::string_view add(std::string_view path) {
    if (bytes_.size() + path.size() > kCapacity) return overflow_.emplace_back(path);
    const size_t at = bytes_.size();
    bytes_.insert(bytes_.end(), path.begin(), path.end());
    return {bytes_.data() + at, path.size()};
  }

  void clear() {
    BigVec<char>().swap(bytes_);
    std::deque<std::string>().swap(overflow_);
  }

 private:
  static constexpr size_t kCapacity = size_t(1) << 30;
  BigVec<char> bytes_;
  std::deque<std::string> overflow_;  // stable addresses past the reserve
};

inline uint64_t time_ns(const timespec& t) { return uint64_t(t.tv_sec) * 1000000000ull + uint64_t(t.tv_nsec); }

}  // namespace content_detail

}  // namespace fplussearch
