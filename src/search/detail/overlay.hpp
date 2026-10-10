#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "search/engine.hpp"

namespace fplussearch::detail {

struct Scan;

int32_t extra_score(const Scan& sc, const Extra& x, std::unordered_map<std::string, int8_t>& priors);

}  // namespace fplussearch::detail
