#pragma once

#include <string>

namespace fplussearch {

// The daemon's socket for the index cached at `cache_file`.
std::string socket_path(const std::string& cache_file);

// A connection to the running daemon, or -1.
int connect_daemon(const std::string& sock);
// Starts `exe serve --root root` in its own session, logging to `log`.
bool spawn_daemon(const std::string& exe, const std::string& root, const std::string& log);
// Sends one request line and reads one response line.
bool roundtrip(int fd, const std::string& request, std::string& response);

}  // namespace fplussearch
