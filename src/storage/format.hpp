#pragma once

#include <cstdint>

namespace fplussearch {

// Header, then (offset, size) per field, then the fields, each 64-byte aligned.
struct Header {
  char magic[8];
  uint64_t count;
  uint64_t meta[8];
};

inline uint64_t align64(uint64_t v) { return (v + 63) & ~uint64_t(63); }

}  // namespace fplussearch
