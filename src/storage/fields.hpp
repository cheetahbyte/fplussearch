#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index/section.hpp"
#include "storage/mapped_array.hpp"

namespace fplussearch {

// A finished field; owns its storage without copying it.
struct Blob {
  std::shared_ptr<const void> owner;
  const char* data = nullptr;
  size_t size = 0;
};

template <typename T>
Blob blob(BigVec<T>&& v) {
  auto owned = std::make_shared<const BigVec<T>>(std::move(v));
  return {owned, reinterpret_cast<const char*>(owned->data()), owned->size() * sizeof(T)};
}

// Fields of a mapped (or in-memory) container.
struct Fields {
  std::array<uint64_t, 8> meta{};
  std::vector<std::pair<const char*, size_t>> f;
  std::shared_ptr<const void> backing;
  size_t bytes = 0;

  template <typename T>
  bool get(size_t i, Span<T>& s) const {
    if (i >= f.size() || reinterpret_cast<uintptr_t>(f[i].first) % alignof(T) || f[i].second % sizeof(T)) return false;
    s.p = reinterpret_cast<const T*>(f[i].first);
    s.n = f[i].second / sizeof(T);
    return true;
  }
  bool section(size_t first, Section& s, bool hex) const;
};

bool save_fields(const std::string& file, const char (&magic)[8], const std::array<uint64_t, 8>& meta,
                 const std::vector<Blob>& fields);

bool map_fields(const std::string& file, const char (&magic)[8], size_t count, Fields& out);
// Serves fields from memory when the cache file cannot be written.
Fields hold_fields(const std::array<uint64_t, 8>& meta, std::vector<Blob> fields);

const Overflow* find_key(const Span<Overflow>& s, uint32_t key);
uint64_t hash_bytes(uint64_t seed, std::string_view s);

}  // namespace fplussearch
