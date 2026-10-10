#pragma once

#include <sys/mman.h>

#include <cstddef>
#include <memory>
#include <new>
#include <vector>

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

}  // namespace fplussearch
