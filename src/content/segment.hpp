#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "content/bloom.hpp"
#include "content/content_index.hpp"
#include "content/postings.hpp"

namespace fplussearch {

inline constexpr int8_t kNotText = INT8_MIN;  // looked at, not text: never a candidate

struct ContentIndex::Segment {
  uint64_t id = 0;
  uint32_t ndocs = 0;
  std::shared_ptr<const void> backing;
  size_t bytes = 0;
  Span<uint32_t> key, off, path_off, mtime, by_path;
  Span<uint8_t> post;
  Span<char> paths;
  Span<uint64_t> size;
  Span<int8_t> rank;
  std::vector<uint32_t> path_order;  // document -> position in by_path
  std::shared_ptr<const std::vector<content_detail::BloomView>> blooms;
  std::shared_ptr<const void> bloom_backing;

  std::string_view path(uint32_t d) const { return {paths.p + path_off[d], path_off[d + 1] - path_off[d]}; }

  // Appends the docs of trigram slot k (ascending) whose follow mask has
  // every bit of `need` to `out`, and their masks to `masks` if given. Dense
  // (bitset) lists carry no masks: they pass every `need`.
  void list(size_t k, std::vector<uint32_t>& out, uint8_t need = 0, std::vector<uint8_t>* masks = nullptr) const {
    const uint32_t a = off[k] & ~content_detail::kBitset, b = off[k + 1] & ~content_detail::kBitset;
    const uint8_t* p = post.p + a;
    if (off[k] & content_detail::kBitset) {
      for (uint32_t w = 0; w < b - a; ++w)
        for (uint8_t x = p[w]; x; x &= uint8_t(x - 1)) {
          out.push_back(w * 8 + uint32_t(__builtin_ctz(x)));
          if (masks) masks->push_back(0x7f);
        }
      return;
    }
    const uint8_t* end = post.p + b;
    uint32_t last = 0, v;
    while (p < end) {
      p = content_detail::get_varint(p, v);
      last += v;
      const uint8_t m = *p++;
      if ((m & need) != need) continue;
      out.push_back(last);
      if (masks) masks->push_back(m);
    }
  }
  size_t find(uint32_t t) const {
    const uint32_t* it = std::lower_bound(key.p, key.p + key.n, t);
    return it != key.p + key.n && *it == t ? size_t(it - key.p) : SIZE_MAX;
  }
  // Keeps the docs of `acc` (ascending) that varint slot k lists with every
  // bit of `need`. Equivalent to list() + set_intersection, but decodes in
  // place without buffers and stops once the list passes acc's last doc.
  void intersect(size_t k, std::vector<uint32_t>& acc, uint8_t need) const {
    const uint8_t* p = post.p + (off[k] & ~content_detail::kBitset);
    const uint8_t* end = post.p + (off[k + 1] & ~content_detail::kBitset);
    const size_t n = acc.size();
    size_t i = 0, w = 0;
    uint32_t last = 0, v;
    while (p < end) {
      p = content_detail::get_varint(p, v);
      last += v;
      const uint8_t m = *p++;
      while (acc[i] < last)
        if (++i == n) goto done;
      if (acc[i] == last) {
        if ((m & need) == need) acc[w++] = last;
        if (++i == n) break;
      }
    }
  done:
    acc.resize(w);
  }
  size_t list_bytes(size_t k) const { return (off[k + 1] & ~content_detail::kBitset) - (off[k] & ~content_detail::kBitset); }
  bool is_bitset(size_t k) const { return off[k] & content_detail::kBitset; }
  bool has(size_t k, uint32_t d) const {  // bitset lists only
    return (post.p[(off[k] & ~content_detail::kBitset) + d / 8] >> (d % 8)) & 1;
  }

  bool may_contain(uint32_t d, const std::vector<std::pair<uint32_t, uint8_t>>& probes) const {
    if (!blooms) return true;  // Segments written before the bloom sidecar.
    const content_detail::BloomView& b = (*blooms)[d];
    for (auto [h, which] : probes)
      if (!content_detail::bloom_has(which ? b.long_bits : b.short_bits, h)) return false;
    return true;
  }
};

struct SegView {
  std::shared_ptr<const ContentIndex::Segment> seg;
  std::shared_ptr<const std::vector<uint64_t>> dead;
  size_t live = 0;
  bool is_dead(uint32_t d) const { return ((*dead)[d >> 6] >> (d & 63)) & 1; }
};

struct ContentIndex::State {
  std::vector<SegView> segs;
};

}  // namespace fplussearch
