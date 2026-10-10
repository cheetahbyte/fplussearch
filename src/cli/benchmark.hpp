#pragma once

#include <string>
#include <vector>

#include "search/engine.hpp"

namespace fplussearch::cli {

Results run_query(const Index& ix, const SymbolIndex* sx, Engine& engine, const Query& q, size_t limit);
int bench(const Index& ix, const SymbolIndex* sx, Engine& engine, std::vector<std::string> queries);

}  // namespace fplussearch::cli
