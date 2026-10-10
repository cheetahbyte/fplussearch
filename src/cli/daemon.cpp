#include "cli/daemon.hpp"
#include "cli/runtime.hpp"
#include "daemon/server.hpp"
#include "daemon/socket.hpp"
#include "live/live.hpp"
#include "support/json.hpp"

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

namespace fplussearch::cli {

// A connection to the daemon for `cache`, starting it if asked and waiting
// up to `wait` for it to answer.
int daemon_connection(const std::string& root, const std::string& cache, bool start, std::chrono::milliseconds wait) {
  const std::string sock = socket_path(cache);
  int fd = connect_daemon(sock);
  if (fd >= 0 || !start) return fd;
  if (!spawn_daemon(exe_path(), root, log_path(cache))) return -1;
  for (auto deadline = Clock::now() + wait; fd < 0 && Clock::now() < deadline;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    fd = connect_daemon(sock);
  }
  return fd;
}

// Prints a daemon's answer the way a local search prints; false if it
// couldn't answer (still indexing, or gone).
bool print_daemon_results(int fd, const std::string& text, size_t limit, int& status) {
  std::string req = "{\"q\":";
  json_escape(req, text);
  req += ",\"limit\":" + std::to_string(limit == SIZE_MAX ? 1000000 : limit) + "}";
  std::string resp;
  JsonValue r;
  if (!roundtrip(fd, req, resp) || !json_parse(resp, r) || !r.flag("ok")) return false;
  if (const JsonValue* hits = r.get("hits"))
    for (const JsonValue& h : hits->items) {
      if (h.get("symbol"))
        std::printf("%s:%u: %s %s\n", h.str("path").c_str(), unsigned(h.num("line")), h.str("kind").c_str(),
                    h.str("symbol").c_str());
      else
        std::printf("%s\n", h.str("path").c_str());
    }
  if (const JsonValue* files = r.get("files")) {
    for (const JsonValue& f : files->items)
      if (const JsonValue* lines = f.get("lines"))
        for (const JsonValue& l : lines->items)
          std::printf("%s:%u: %s\n", f.str("path").c_str(), unsigned(l.num("line")), l.str("text").c_str());
    std::fprintf(stderr, "%zu files in %.3f ms (daemon%s)\n", files->items.size(), r.num("ms"),
                 r.flag("complete") ? "" : ", time budget reached");
    status = files->items.empty() ? 1 : 0;
    return true;
  }
  if (!r.str("warning").empty()) std::fprintf(stderr, "%s\n", r.str("warning").c_str());
  const double total = r.num("total");
  std::fprintf(stderr, "%.0f matches in %.3f ms (daemon)\n", total, r.num("ms"));
  status = total > 0 ? 0 : 1;
  return true;
}

// Pipes JSON lines between stdin/stdout and the daemon.
int stdio(int fd) {
  std::thread([fd] {
    char buf[65536];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) {
      if (std::fwrite(buf, 1, size_t(n), stdout) != size_t(n)) break;
      std::fflush(stdout);
    }
    std::exit(0);
  }).detach();
  char buf[65536];
  for (ssize_t n; (n = read(STDIN_FILENO, buf, sizeof buf)) > 0;)
    for (ssize_t off = 0; off < n;) {
      const ssize_t w = write(fd, buf + off, size_t(n - off));
      if (w <= 0) return 1;
      off += w;
    }
  shutdown(fd, SHUT_WR);
  std::this_thread::sleep_for(std::chrono::seconds(30));  // the reader exits once answers are out
  return 0;
}

namespace {
char g_sock[1024];
}

int serve_daemon(const std::string& root, const std::string& cache, unsigned threads, unsigned scan_threads) {
  std::shared_ptr<const Index> ix;
  auto loaded = std::make_shared<Index>();
  if (load_index(*loaded, cache) && loaded->root == root) ix = loaded;
  Engine engine(threads);
  Live live({.root = root, .cache_file = cache, .scan_threads = scan_threads}, engine);
  const std::string sock = socket_path(cache);
  std::snprintf(g_sock, sizeof g_sock, "%s", sock.c_str());
  struct sigaction sa {};
  sa.sa_handler = [](int) {
    unlink(g_sock);
    _exit(0);
  };
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);
  if (int fd = connect_daemon(sock); fd >= 0) {
    close(fd);
    std::fprintf(stderr, "a daemon already serves %s\n", root.c_str());
    return 1;
  }
  live.start(ix);
  std::fprintf(stderr, "serving %s on %s\n", root.c_str(), sock.c_str());
  if (!serve(live, sock, home())) {
    std::fprintf(stderr, "cannot serve on %s\n", sock.c_str());
    return 1;
  }
  return 0;
}

}  // namespace fplussearch::cli
