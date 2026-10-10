#pragma once

#include <string>
#include <string_view>

namespace fplussearch {

class Live;

// Answers one request (one JSON object) with one JSON line, no newline.
// Requests: {"q": "query", "limit": n}, {"op": "grep", "q": ..., "pattern":
// ..., "mode": "literal"|"regex", "limit": n, "per_file": n, "budget_ms": n}
// (a q with grep: or regex: is a grep too), {"op": "status"}. An "id" is
// echoed back.
std::string handle_request(Live& live, std::string_view line, const std::string& home);

}  // namespace fplussearch
