#pragma once

#include <string>

namespace fplussearch {

class Live;

// Serves requests on `sock` until the process exits; false if the socket
// can't be bound (or another daemon already serves it).
bool serve(Live& live, const std::string& sock, const std::string& home);

}  // namespace fplussearch
