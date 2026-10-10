#pragma once

#include "index/metadata.hpp"
#include "storage/fields.hpp"

namespace fplussearch {

constexpr size_t kSectionFields = 7;

// Lays out names [0, count) as a Section. `first` has count + 1 entries: the
// first item (entry or occurrence) of each name, then the end.
template <typename NameAt>
std::array<Blob, kSectionFields> make_section(size_t count, NameAt&& name_at, const uint32_t* first, bool hex);

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
