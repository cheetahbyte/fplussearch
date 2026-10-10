#include "content/bloom.hpp"

#include <algorithm>

namespace fplussearch::content_detail {

void make_bloom(std::string_view s, Bloom& b) {
  std::vector<uint32_t> h[2];
  for (size_t n : {size_t(5), size_t(7)}) {
    auto& v = h[n == 5 ? 0 : 1];
    for (size_t i = 0; i + n <= s.size(); ++i) v.push_back(gram_hash(s.data() + i, n));
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    auto& bits = n == 5 ? b.short_bits : b.long_bits;
    bits.assign(std::max<size_t>(1, (v.size() * 3 + 63) / 64), 0);
    for (uint32_t x : v) bloom_set(bits, x);
  }
}

}  // namespace fplussearch::content_detail
