#include "storage/interner.hpp"

#include <cstring>

namespace fplussearch {

uint32_t Interner::intern(std::string_view s, uint32_t hash) {
  if (slots_.empty()) slots_.assign(size_t(1) << 20, 0);
  const size_t mask = slots_.size() - 1;
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    const uint32_t slot = slots_[i];
    if (slot == 0) break;
    if (hash_[slot - 1] == hash && get(slot - 1) == s) return slot - 1;
  }
  const uint32_t id = uint32_t(ptr_.size());
  if (arena_.empty() || used_ + s.size() > kMapChunk) {
    arena_.push_back(map_chunk());
    used_ = 0;
  }
  char* dst = static_cast<char*>(arena_.back().get()) + used_;
  std::memcpy(dst, s.data(), s.size());
  used_ += s.size();
  ptr_.push_back(dst);
  len_.push_back(uint16_t(s.size()));
  hash_.push_back(hash);
  insert(id);
  if (ptr_.size() * 2 > slots_.size()) grow();
  return id;
}

void Interner::insert(uint32_t id) {
  const size_t mask = slots_.size() - 1;
  size_t i = hash_[id] & mask;
  while (slots_[i]) i = (i + 1) & mask;
  slots_[i] = id + 1;
}

void Interner::grow() {
  slots_.assign(slots_.size() * 2, 0);
  for (uint32_t id = 0; id < ptr_.size(); ++id) insert(id);
}

}  // namespace fplussearch
