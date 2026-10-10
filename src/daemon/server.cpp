#include "daemon/server.hpp"
#include "daemon/protocol.hpp"
#include "daemon/socket.hpp"

#include <fcntl.h>
#include <malloc/malloc.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <thread>

namespace fplussearch {

bool serve(Live& live, const std::string& sock, const std::string& home) {
  if (int fd = connect_daemon(sock); fd >= 0) {  // another daemon serves this index
    close(fd);
    return false;
  }
  const int ls = socket(AF_UNIX, SOCK_STREAM, 0);
  if (ls < 0) return false;
  fcntl(ls, F_SETFD, FD_CLOEXEC);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (sock.size() >= sizeof addr.sun_path) return false;
  std::strcpy(addr.sun_path, sock.c_str());
  unlink(sock.c_str());
  const mode_t old = umask(077);  // only this user may connect
  const bool bound = bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
  umask(old);
  if (!bound || listen(ls, 64) != 0) return false;
  for (;;) {
    const int c = accept(ls, nullptr, nullptr);
    if (c < 0) continue;
    const int no_sigpipe = 1;
    if (setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof no_sigpipe) != 0) {
      close(c);
      continue;
    }
    std::thread([&live, &home, c] {
      std::string buf;
      char chunk[65536];
      for (;;) {
        const ssize_t n = read(c, chunk, sizeof chunk);
        if (n <= 0) break;
        buf.append(chunk, size_t(n));
        for (size_t nl; (nl = buf.find('\n')) != std::string::npos;) {
          std::string resp = handle_request(live, std::string_view(buf).substr(0, nl), home);
          buf.erase(0, nl + 1);
          if (buf.capacity() > (64 << 10) && buf.size() < buf.capacity() / 2) {
            std::string(buf).swap(buf);
          }
          resp += '\n';
          for (size_t off = 0; off < resp.size();) {
            const ssize_t w = write(c, resp.data() + off, resp.size() - off);
            if (w <= 0) break;
            off += size_t(w);
          }
          const bool large_response = resp.size() > (64 << 10);
          if (large_response) {
            std::string().swap(resp);
            malloc_zone_pressure_relief(nullptr, 0);
          }
        }
      }
      close(c);
    }).detach();
  }
}

}  // namespace fplussearch
