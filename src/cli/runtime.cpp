#include "cli/runtime.hpp"

#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <sys/sysctl.h>

#include <algorithm>
#include <cstdlib>
#include <thread>

namespace fplussearch::cli {

// Searches split evenly across threads, so efficiency cores, which finish
// their share last, would only slow them down.
unsigned performance_cores() {
  int n = 0;
  size_t len = sizeof n;
  if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &len, nullptr, 0) == 0 && n > 0) return unsigned(n);
  return std::max(1u, std::thread::hardware_concurrency());
}

std::string home() {
  const char* h = std::getenv("HOME");
  return h ? h : "";
}

std::string cache_path(const std::string& root) {
  const char* home = std::getenv("HOME");
  std::string dir = std::string(home ? home : "/tmp") + "/Library/Caches/fplussearch";
  mkdir(dir.c_str(), 0755);
  std::string key = root;
  for (auto& c : key)
    if (c == '/') c = '_';
  return dir + "/index" + key + ".bin";
}

std::string exe_path() {
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) != 0) return {};
  char* real = realpath(buf, nullptr);
  std::string out = real ? real : buf;
  std::free(real);
  return out;
}

std::string log_path(const std::string& cache) { return cache.substr(0, cache.rfind('/')) + "/daemon.log"; }

}  // namespace fplussearch::cli
