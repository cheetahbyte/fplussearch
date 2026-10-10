#pragma once

#include <cstdint>
#include <string_view>

#include "search/query.hpp"

namespace fplussearch {

// Whether a file path passes the query's name words, ext:, type: (by
// extension), re: and path: — the filters content search applies to the
// files it reads. Size, date and in: are left to the caller.
bool path_matches(const Query& q, std::string_view path);

// Case-insensitive substring scoring, shared with other match sources.
int32_t fuzzy_score(std::string_view name, std::string_view q);
// Score of one token against a name with name_mask `m`, or INT32_MIN.
int32_t token_score(std::string_view name, uint64_t m, const Token& t);
constexpr int32_t kNoMatch = INT32_MIN;

}  // namespace fplussearch
