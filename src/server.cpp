#include "server.hpp"

#include <fcntl.h>
#include <malloc/malloc.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "store.hpp"
#include "symindex.hpp"

extern char** environ;

namespace fplussearch {

namespace {

std::string number(double v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.3f", v);
  return buf;
}

void hit_json(std::string& out, const Live::State& s, size_t i, const Results& r) {
  const Index& ix = *s.ix;
  out += "{\"path\":";
  if (const Extra* x = r.extra[i]) {
    json_escape(out, x->path);
    out += ",\"dir\":" + std::string(x->dir ? "true" : "false");
    out += ",\"size\":" + std::to_string(x->size) + ",\"mtime\":" + std::to_string(x->mtime);
  } else {
    const uint32_t e = r.top[i];
    json_escape(out, ix.path(e));
    out += ",\"dir\":" + std::string(ix.is_dir(e) ? "true" : "false");
    out += ",\"size\":" + std::to_string(ix.is_dir(e) ? 0 : ix.size(e)) + ",\"mtime\":" + std::to_string(ix.mtime[e]);
  }
  out += ",\"score\":" + std::to_string(r.score[i]) + "}";
}

void symbols_json(std::string& out, const Live::State& s, const Results& r) {
  out += ",\"hits\":[";
  for (size_t i = 0; i < r.top.size(); ++i) {
    const uint32_t o = r.top[i];
    const std::string path = s.ix->path(s.sx->occ[o]);
    const std::string_view name = s.sx->symbol(o);
    const SymbolLocation loc = locate_symbol(path, name);
    if (i) out += ',';
    out += "{\"path\":";
    json_escape(out, path);
    out += ",\"line\":" + std::to_string(loc.line) + ",\"symbol\":";
    json_escape(out, name);
    out += ",\"kind\":";
    json_escape(out, loc.found ? kind_name(loc.kind) : "?");
    out += '}';
  }
  out += ']';
}

void status_json(std::string& out, const Live& live) {
  const Live::Status st = live.status();
  out += ",\"ready\":" + std::string(st.ready ? "true" : "false");
  out += ",\"entries\":" + std::to_string(st.entries) + ",\"dirs\":" + std::to_string(st.dirs);
  out += ",\"overlay\":" + std::to_string(st.overlay) + ",\"removed\":" + std::to_string(st.removed);
  out += ",\"symbols\":" + std::to_string(st.symbols) + ",\"event_id\":" + std::to_string(st.event_id);
  out += ",\"owner\":" + std::string(st.owner ? "true" : "false");
  out += ",\"busy\":" + std::string(st.busy ? "true" : "false");
  out += ",\"full_disk_access\":" + std::string(has_full_disk_access() ? "true" : "false");
  out += ",\"content_docs\":" + std::to_string(st.content_docs);
  out += ",\"content_bytes\":" + std::to_string(st.content_bytes);
  out += ",\"content_pending\":" + std::string(st.content_pending ? "true" : "false");
}

std::string error_json(std::string_view msg, const std::string& id) {
  std::string out = "{\"ok\":false" + id + ",\"error\":";
  json_escape(out, msg);
  return out + "}";
}

}  // namespace

std::string socket_path(const std::string& cache_file) {
  // sun_path holds 104 bytes, so name the socket by a hash of the cache file.
  char name[32];
  std::snprintf(name, sizeof name, "/d%012llx.sock",
                (unsigned long long)(hash_bytes(0, cache_file) & 0xffffffffffffull));
  return cache_file.substr(0, cache_file.rfind('/')) + name;
}

std::string handle_request(Live& live, std::string_view line, const std::string& home) {
  JsonValue req;
  if (!json_parse(line, req) || req.type != JsonValue::Object)
    return error_json("bad request: expected a JSON object", "");
  std::string id;
  if (const JsonValue* v = req.get("id")) {
    id = ",\"id\":";
    if (v->type == JsonValue::String) json_escape(id, v->s);
    else id += number(v->n);
  }
  auto str = [&](std::string_view k) { return req.str(k); };
  auto num = [&](std::string_view k, double d) { return req.num(k, d); };
  const std::string op = str("op");
  std::string out = "{\"ok\":true" + id;
  if (op == "status") {
    status_json(out, live);
    return out + "}";
  }
  const auto s = live.state();
  if (!s) return error_json("indexing (the first run scans the whole disk)", id);
  const Query q = parse_query(str("q"), home);
  if (op == "grep" || !q.grep.empty()) {
    const ContentIndex* content = live.content();
    if (!content) return error_json("content search is off", id);
    GrepOptions o;
    o.pattern = req.str("pattern").empty() ? q.grep : req.str("pattern");
    if (o.pattern.empty()) return error_json("grep needs a pattern", id);
    o.regex = req.get("mode") ? req.str("mode") == "regex" : q.grep_mode == GrepMode::Regex;
    o.limit = size_t(num("limit", q.limit ? double(q.limit) : 50));
    o.per_file = size_t(num("per_file", 5));
    o.budget_ms = int(num("budget_ms", 250));
    o.scope = q.scope;
    Query filters = q;
    filters.grep.clear();
    if (!filters.tokens.empty() || !filters.exts.empty() || filters.types != kTypeAny || filters.name_re ||
        filters.path_re)
      o.keep = [filters](std::string_view path) { return path_matches(filters, path); };
    const auto t0 = std::chrono::steady_clock::now();
    GrepResult g;
    if (o.scope.empty() || content->covers(o.scope)) {
      g = content->grep(o);
    } else {
      // Outside the content index (in:/etc): read the matching files directly.
      g = content->grep_files(o, live.files(*s, filters, 200000));
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!g.error.empty()) return error_json(g.error, id);
    out += ",\"ms\":" + number(ms) + ",\"candidates\":" + std::to_string(g.candidates) +
           ",\"read\":" + std::to_string(g.read) + ",\"complete\":" + (g.complete ? "true" : "false") + ",\"files\":[";
    for (size_t i = 0; i < g.files.size(); ++i) {
      if (i) out += ',';
      out += "{\"path\":";
      json_escape(out, g.files[i].path);
      out += ",\"lines\":[";
      for (size_t j = 0; j < g.files[i].lines.size(); ++j) {
        if (j) out += ',';
        out += "{\"line\":" + std::to_string(g.files[i].lines[j].line) + ",\"text\":";
        json_escape(out, g.files[i].lines[j].text);
        out += '}';
      }
      out += "]}";
    }
    return out + "]}";
  }
  if (!op.empty()) return error_json("unknown op " + op, id);
  if (!q.error.empty()) {
    out += ",\"warning\":";
    json_escape(out, q.error);
  }
  const size_t limit = size_t(num("limit", q.limit ? double(q.limit) : 50));
  const auto t0 = std::chrono::steady_clock::now();
  const Results r = live.search(*s, q, limit);
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  out += ",\"total\":" + std::to_string(r.total) + ",\"ms\":" + number(ms);
  if (!q.syms.empty()) {
    if (s->sx) symbols_json(out, *s, r);
    else out += ",\"hits\":[]";
    return out + "}";
  }
  out += ",\"hits\":[";
  for (size_t i = 0; i < r.top.size(); ++i) {
    if (i) out += ',';
    hit_json(out, *s, i, r);
  }
  return out + "]}";
}

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
