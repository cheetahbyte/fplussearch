#pragma once

#include <cstdint>
#include <string_view>

#include "storage/chunked.hpp"

namespace fplussearch {

// Deduplicates strings into dense ids. Not thread-safe.
class Interner {
 public:
  uint32_t intern(std::string_view s, uint32_t hash);
  std::string_view get(uint32_t id) const { return {ptr_[id], len_[id]}; }
  size_t size() const { return ptr_.size(); }
  void drop_table() { BigVec<uint32_t>().swap(slots_); }

 private:
  void insert(uint32_t id);
  void grow();

  std::vector<MapChunk> arena_;
  size_t used_ = 0;
  Chunked<const char*> ptr_;
  Chunked<uint16_t> len_;
  Chunked<uint32_t> hash_;
  BigVec<uint32_t> slots_;
};

}  // namespace fplussearch
