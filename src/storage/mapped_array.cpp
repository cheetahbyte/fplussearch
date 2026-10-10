#include "storage/mapped_array.hpp"

namespace fplussearch {

MapChunk map_chunk() {
  void* p = mmap(nullptr, kMapChunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  return MapChunk(p);
}

}  // namespace fplussearch
