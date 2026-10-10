#pragma once

#include "storage/mapped_array.hpp"

namespace fplussearch {

// Append-only array in fixed chunks: growth never copies.
template <typename T>
class Chunked {
 public:
  void push_back(const T& v) {
    if (n_ % kChunk == 0) chunks_.push_back(map_chunk());
    static_cast<T*>(chunks_.back().get())[n_ % kChunk] = v;
    ++n_;
  }
  const T& operator[](size_t i) const { return static_cast<const T*>(chunks_[i / kChunk].get())[i % kChunk]; }
  size_t size() const { return n_; }
  void clear() {
    std::vector<MapChunk>().swap(chunks_);
    n_ = 0;
  }

 private:
  static constexpr size_t kChunk = kMapChunk / sizeof(T);
  std::vector<MapChunk> chunks_;
  size_t n_ = 0;
};

}  // namespace fplussearch
