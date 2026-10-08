#pragma once

// Building blocks shared by the file index and the symbol index: build-time
// buffers, the name table, the section writer, and the on-disk container.

#include <sys/mman.h>

#include <array>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "index.hpp"

namespace fplussearch {

// 4 MB of anonymous memory. Mapped directly because malloc keeps freed
// medium-sized blocks resident, and index builds free hundreds of MB.
constexpr size_t kMapChunk = size_t(4) << 20;
struct Unmap {
  void operator()(void* p) const { munmap(p, kMapChunk); }
};
using MapChunk = std::unique_ptr<void, Unmap>;
MapChunk map_chunk();

// Allocates large buffers straight from the VM system. malloc keeps freed
// large blocks dirty in a reuse cache, which would keep a finished build's
// temporaries in the process footprint.
template <typename T>
struct MapAlloc {
  using value_type = T;
  static constexpr size_t kDirect = size_t(256) << 10;
  MapAlloc() = default;
  template <typename U>
  MapAlloc(const MapAlloc<U>&) {}
  T* allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
    if (bytes < kDirect) return static_cast<T*>(::operator new(bytes));
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    return static_cast<T*>(p);
  }
  void deallocate(T* p, size_t n) {
    const size_t bytes = n * sizeof(T);
    if (bytes < kDirect) ::operator delete(p);
    else munmap(p, bytes);
  }
  template <typename U>
  bool operator==(const MapAlloc<U>&) const { return true; }
};

template <typename T>
using BigVec = std::vector<T, MapAlloc<T>>;

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

// Deduplicates strings into dense ids. Not thread-safe.
class Interner {
 public:
  uint32_t intern(std::string_view s, uint32_t hash);
  std::string_view get(uint32_t id) const { return {ptr_[id], len_[id]}; }
  size_t size() const { return ptr_.size(); }
  void drop_table() { slots_ = {}; }

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

constexpr size_t kSectionFields = 7;

// Lays out names [0, count) as a Section. `first` has count + 1 entries: the
// first item (entry or occurrence) of each name, then the end.
template <typename NameAt>
std::array<Blob, kSectionFields> make_section(size_t count, NameAt&& name_at, const uint32_t* first, bool hex);

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

// Writes the same container as save_fields, but field by field and in any
// order, so a big field can be streamed out instead of held in memory.
class FieldWriter {
 public:
  FieldWriter(std::string file, const char (&magic)[8], size_t count);
  ~FieldWriter();
  FieldWriter(const FieldWriter&) = delete;
  FieldWriter& operator=(const FieldWriter&) = delete;

  bool write(size_t field, const void* data, size_t size);
  // A field written in pieces: begin, append..., end.
  bool begin(size_t field);
  bool append(const void* data, size_t size);
  void end();
  // Writes the header and publishes the file; false if anything failed.
  bool finish(const std::array<uint64_t, 8>& meta);

 private:
  std::string file_, tmp_;
  char magic_[8];
  FILE* f_ = nullptr;
  std::vector<uint64_t> table_;  // (offset, size) per field
  uint64_t pos_ = 0;
  size_t open_ = SIZE_MAX;  // the field being appended to
  bool ok_ = true;
};
bool map_fields(const std::string& file, const char (&magic)[8], size_t count, Fields& out);
// Serves fields from memory when the cache file cannot be written.
Fields hold_fields(const std::array<uint64_t, 8>& meta, std::vector<Blob> fields);

const Overflow* find_key(const Span<Overflow>& s, uint32_t key);
uint64_t hash_bytes(uint64_t seed, std::string_view s);

bool is_hex_name(std::string_view s);
void pack_hex(std::string_view s, BigVec<char>& out);

template <typename NameAt>
std::array<Blob, kSectionFields> make_section(size_t count, NameAt&& name_at, const uint32_t* first, bool hex) {
  BigVec<char> bytes;
  BigVec<uint32_t> block_off, block_first;
  BigVec<uint16_t> name_off;
  BigVec<uint8_t> counts;
  BigVec<Overflow> overflow;
  BigVec<uint64_t> mask(count);
  size_t total = kPad;
  for (size_t k = 0; k < count; ++k) {
    const size_t len = name_at(k).size();
    total += hex ? 1 + (len + 1) / 2 : len + 1;
  }
  bytes.reserve(total);
  block_off.reserve(count / kBlock + 2);
  block_first.reserve(count / kBlock + 2);
  name_off.reserve(count);
  counts.reserve(count);
  size_t block_start = 0;
  for (size_t k = 0; k < count; ++k) {
    if (k % kBlock == 0) {
      block_start = bytes.size();
      block_off.push_back(uint32_t(block_start));
      block_first.push_back(first[k]);
    }
    name_off.push_back(uint16_t(bytes.size() - block_start));
    const uint32_t c = first[k + 1] - first[k];
    counts.push_back(uint8_t(std::min<uint32_t>(c, 255)));
    if (c >= 255) overflow.push_back({uint32_t(k), 0, c});
    const std::string_view nm = name_at(k);
    mask[k] = name_mask(nm);
    if (hex) {
      pack_hex(nm, bytes);
    } else {
      bytes.insert(bytes.end(), nm.begin(), nm.end());
      bytes.push_back('\0');
    }
  }
  block_off.push_back(uint32_t(bytes.size()));
  block_first.push_back(first[count]);
  bytes.resize(bytes.size() + kPad, '\0');
  return {blob(std::move(bytes)), blob(std::move(block_off)), blob(std::move(block_first)),
          blob(std::move(name_off)), blob(std::move(counts)), blob(std::move(overflow)), blob(std::move(mask))};
}

}  // namespace fplussearch
