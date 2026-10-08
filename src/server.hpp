#pragma once

#include <functional>
#include <string>

#include "json.hpp"
#include "live.hpp"

namespace fplussearch {

// The daemon's socket for the index cached at `cache_file`.
std::string socket_path(const std::string& cache_file);

// Answers one request (one JSON object) with one JSON line, no newline.
// Requests: {"q": "query", "limit": n}, {"op": "grep", "q": ..., "pattern":
// ..., "mode": "literal"|"regex", "limit": n, "per_file": n, "budget_ms": n}
// (a q with grep: or regex: is a grep too), {"op": "status"}. An "id" is
// echoed back.
std::string handle_request(Live& live, std::string_view line, const std::string& home);

// Serves requests on `sock` until the process exits; false if the socket
// can't be bound (or another daemon already serves it).
bool serve(Live& live, const std::string& sock, const std::string& home);

// A connection to the running daemon, or -1.
int connect_daemon(const std::string& sock);
// Starts `exe serve --root root` in its own session, logging to `log`.
bool spawn_daemon(const std::string& exe, const std::string& root, const std::string& log);
// Sends one request line and reads one response line.
bool roundtrip(int fd, const std::string& request, std::string& response);

}  // namespace fplussearch
