#pragma once

#include <chrono>
#include <string>

namespace fplussearch::cli {

using Clock = std::chrono::steady_clock;

inline double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

unsigned performance_cores();
std::string home();
std::string cache_path(const std::string& root);
std::string exe_path();
std::string log_path(const std::string& cache);

}  // namespace fplussearch::cli
