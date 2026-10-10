#include "daemon/socket.hpp"
#include "storage/fields.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>

extern char** environ;

namespace fplussearch {

std::string socket_path(const std::string& cache_file) {
  // sun_path holds 104 bytes, so name the socket by a hash of the cache file.
  char name[32];
  std::snprintf(name, sizeof name, "/d%012llx.sock",
                (unsigned long long)(hash_bytes(0, cache_file) & 0xffffffffffffull));
  return cache_file.substr(0, cache_file.rfind('/')) + name;
}

int connect_daemon(const std::string& sock) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (sock.size() >= sizeof addr.sun_path) {
    close(fd);
    return -1;
  }
  std::strcpy(addr.sun_path, sock.c_str());
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  return fd;
}

bool spawn_daemon(const std::string& exe, const std::string& root, const std::string& log) {
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);  // closing the terminal doesn't stop it
  const char* argv[] = {exe.c_str(), "serve", "--root", root.c_str(), nullptr};
  pid_t pid;
  const bool ok = posix_spawn(&pid, exe.c_str(), &fa, &attr, const_cast<char**>(argv), environ) == 0;
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  return ok;
}

bool roundtrip(int fd, const std::string& request, std::string& response) {
  const std::string line = request + "\n";
  for (size_t off = 0; off < line.size();) {
    const ssize_t w = write(fd, line.data() + off, line.size() - off);
    if (w <= 0) return false;
    off += size_t(w);
  }
  response.clear();
  char c[65536];
  for (;;) {
    const ssize_t n = read(fd, c, sizeof c);
    if (n <= 0) return false;
    response.append(c, size_t(n));
    if (response.back() == '\n') {
      response.pop_back();
      return true;
    }
  }
}

}  // namespace fplussearch
