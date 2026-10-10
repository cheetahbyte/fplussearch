#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace fplussearch::cli {

int daemon_connection(const std::string& root, const std::string& cache, bool start, std::chrono::milliseconds wait);
bool print_daemon_results(int fd, const std::string& text, size_t limit, int& status);
int stdio(int fd);
int serve_daemon(const std::string& root, const std::string& cache, unsigned threads, unsigned scan_threads);

}  // namespace fplussearch::cli
