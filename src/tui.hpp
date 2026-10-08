#pragma once

#include <memory>
#include <string>

#include "index.hpp"
#include "search.hpp"

namespace fplussearch {

struct TuiOptions {
  std::string root;
  std::string cache_file;
  unsigned scan_threads;
  bool rescan;  // refresh a cached index in the background
};

// Returns the selected path, or an empty string if the user quit.
std::string run_tui(std::shared_ptr<const Index> ix, Engine& engine, const TuiOptions& opt);

}  // namespace fplussearch
