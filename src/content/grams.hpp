#pragma once

#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

#include "index/index.hpp"
#include "storage/mapped_array.hpp"

namespace fplussearch::content_detail {

inline uint8_t fold(uint8_t b) { return kFold[b]; }

// Each posting carries which of 7 buckets the character after the trigram
// falls in, anywhere in the doc: nearly 4-gram precision for a byte.
inline uint8_t follow_bit(uint8_t folded) { return uint8_t(1u << (folded % 7)); }

// A pattern's trigrams with the follow bits every doc must have.
// PCRE2 UTF caseless matching also equates K/S with Kelvin sign/long S.
inline bool unicode_case_byte(uint8_t c) { return fold(c) == 'k' || fold(c) == 's'; }

void trigrams(const char* buf, size_t n, std::vector<uint32_t>& scratch, BigVec<uint32_t>& out);
std::vector<std::pair<uint32_t, uint8_t>> trigrams_small(std::string_view s, bool unicode_caseless = false);

}  // namespace fplussearch::content_detail
