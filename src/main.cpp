#include <mach-o/dyld.h>
#include <mach/mach.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "index.hpp"
#include "json.hpp"
#include "live.hpp"
#include "search.hpp"
#include "server.hpp"
#include "symindex.hpp"
#include "tui.hpp"

using namespace fplussearch;

extern char** environ;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void usage() {
  std::fprintf(stderr,
               "usage: fplussearch [options] [query]\n"
               "       fplussearch serve | stdio | status | install [--login] | uninstall\n"
               "\n"
               "  no query       interactive search; Enter prints the selected path\n"
               "  query          print matching paths and exit (asks the daemon when it runs,\n"
               "                 otherwise answers from the cache and starts the daemon)\n"
               "  serve          run the daemon in the foreground: keeps the index current and\n"
               "                 answers JSON lines on a socket\n"
               "  stdio          JSON lines on stdin/stdout, through the daemon\n"
               "  status         the daemon's status\n"
               "  install        copy to ~/.local/bin; --login also starts the daemon at login\n"
               "  uninstall      remove the login item (keeps the index)\n"
               "\n"
               "  --root PATH    directory to index (default /)\n"
               "  --reindex      ignore the cached index and rescan first\n"
               "  --no-rescan    use the cached index without following changes\n"
               "  --no-daemon    don't use or start the daemon\n"
               "  --threads N    search threads (default: performance cores)\n"
               "  --scan-threads N  indexing threads (default: 8)\n"
               "  -n N           max results to print (default 50, 0 = all)\n"
               "  --bench        time each query (or a default set) over many runs\n"
               "\n"
               "query syntax: words match names fuzzily (5+ letters forgive one typo), in any\n"
               "  folder of the path; 'exact ^prefix suffix$ !exclude \"with space\"\n"
               "  ext:mp4,mov type:video|audio|image|doc|archive|code|font|app|dir|file\n"
               "  kind:file|dir|link in:~/path size:>1gb mtime:<7d re:regex path:regex limit:N\n"
               "  sym:name (definitions in source files) grep:text regex:pattern (file contents)\n");
}

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

constexpr const char* kAgent = "com.fplussearch.daemon";

int run_launchctl(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("/bin/launchctl"));
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, "/bin/launchctl", nullptr, nullptr, argv.data(), environ) != 0) return -1;
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int install(bool login) {
  const std::string bin_dir = home() + "/.local/bin", bin = bin_dir + "/fplussearch";
  mkdir((home() + "/.local").c_str(), 0755);
  mkdir(bin_dir.c_str(), 0755);
  std::error_code ec;
  std::filesystem::copy_file(exe_path(), bin, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::fprintf(stderr, "copy to %s: %s\n", bin.c_str(), ec.message().c_str());
    return 1;
  }
  std::printf("installed %s\n", bin.c_str());
  if (!login) return 0;
  const std::string agents = home() + "/Library/LaunchAgents", plist = agents + "/" + kAgent + ".plist";
  mkdir(agents.c_str(), 0755);
  const std::string log = log_path(cache_path("/"));
  FILE* f = std::fopen(plist.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", plist.c_str());
    return 1;
  }
  std::fprintf(f,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict>\n"
               "  <key>Label</key><string>%s</string>\n"
               "  <key>ProgramArguments</key><array><string>%s</string><string>serve</string></array>\n"
               "  <key>RunAtLoad</key><true/>\n"
               "  <key>KeepAlive</key><true/>\n"
               "  <key>ProcessType</key><string>Background</string>\n"
               "  <key>StandardOutPath</key><string>%s</string>\n"
               "  <key>StandardErrorPath</key><string>%s</string>\n"
               "</dict></plist>\n",
               kAgent, bin.c_str(), log.c_str(), log.c_str());
  std::fclose(f);
  const std::string domain = "gui/" + std::to_string(getuid());
  run_launchctl({"bootout", domain + "/" + kAgent});
  if (run_launchctl({"bootstrap", domain, plist}) != 0) {
    std::fprintf(stderr, "launchctl bootstrap failed; the agent starts at next login\n");
    return 1;
  }
  std::printf("the daemon now starts at login (%s)\n", plist.c_str());
  std::printf("give %s Full Disk Access in System Settings > Privacy & Security to index protected folders\n",
              bin.c_str());
  return 0;
}

int uninstall() {
  const std::string plist = home() + "/Library/LaunchAgents/" + kAgent + ".plist";
  run_launchctl({"bootout", "gui/" + std::to_string(getuid()) + "/" + kAgent});
  std::remove(plist.c_str());
  std::printf("removed the login item; the index stays in %s\n",
              cache_path("/").substr(0, cache_path("/").rfind('/')).c_str());
  return 0;
}

char g_sock[1024];

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

std::shared_ptr<const Index> build(const std::string& root, const std::string& cache, unsigned threads) {
  auto t0 = Clock::now();
  auto ix = std::make_shared<Index>(build_index(root, threads, nullptr, false, cache));
  std::fprintf(stderr, "indexed %zu entries in %.2f s\n", ix->count(), ms_since(t0) / 1000);
  return ix;
}

// Physical footprint excludes clean pages of the mapped index, which the OS
// can drop and re-read at any time; resident size includes them.
void print_memory() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &n) != KERN_SUCCESS) return;
  std::printf("process: %.1f MB footprint, %.1f MB resident\n", info.phys_footprint / 1e6,
              info.resident_size / 1e6);
}

Results run_query(const Index& ix, const SymbolIndex* sx, Engine& engine, const Query& q, size_t limit) {
  if (q.syms.empty()) return engine.search(ix, q, limit);
  return sx ? engine.search_symbols(ix, *sx, q, limit) : Results{};
}

int bench(const Index& ix, const SymbolIndex* sx, Engine& engine, std::vector<std::string> queries) {
  if (queries.empty()) {
    queries = {"size:>1gb type:video", "a", "e", "readme", "config.json", "ext:mp4", "type:image",
               "lib", "node_modules", "zzqxj", "png ico", "size:>100mb"};
    if (sx) queries.insert(queries.end(), {"sym:parse", "sym:render ext:tsx", "sym:main type:file", "sym:x"});
  }
  std::printf("%zu entries (%u directories), index %.1f MB\n", ix.count(), ix.dirs, ix.bytes / 1e6);
  if (sx) std::printf("%zu symbol definitions, symbol index %.1f MB\n", sx->occ.n, sx->bytes / 1e6);
  // hot: back to back. typing: 80 ms apart, like keystrokes. cold: 500 ms
  // apart, after the workers have gone to sleep and the cores have idled.
  std::printf("%-24s %10s %10s %10s %10s\n", "query", "matches", "hot ms", "typing ms", "cold ms");
  auto timed = [&](const std::string& qs, Results& r) {
    auto t0 = Clock::now();
    Query q = parse_query(qs, home());
    r = run_query(ix, sx, engine, q, 50);
    return ms_since(t0);
  };
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  for (const auto& qs : queries) {
    std::vector<double> hot, typing, cold;
    Results r;
    for (int i = 0; i < 40; ++i) hot.push_back(timed(qs, r));
    for (int i = 0; i < 11; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      typing.push_back(timed(qs, r));
    }
    for (int i = 0; i < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      cold.push_back(timed(qs, r));
    }
    std::printf("%-24s %10llu %10.3f %10.3f %10.3f\n", qs.c_str(), (unsigned long long)r.total,
                median(hot), median(typing), median(cold));
  }
  print_memory();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::string root = "/";
  bool reindex = false, rescan = true, do_bench = false, use_daemon = true, login = false;
  unsigned threads = performance_cores();
  unsigned scan_threads = 8;  // APFS stops scaling around here
  size_t limit = 50;
  std::string command;
  std::vector<std::string> words;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--root") root = value();
    else if (a == "--reindex") reindex = true;
    else if (a == "--no-rescan") rescan = false;
    else if (a == "--no-daemon") use_daemon = false;
    else if (a == "--login") login = true;
    else if (a == "--threads") threads = unsigned(std::max(1, std::atoi(value().c_str())));
    else if (a == "--scan-threads") scan_threads = unsigned(std::max(1, std::atoi(value().c_str())));
    else if (a == "-n") limit = size_t(std::atoll(value().c_str()));
    else if (a == "--bench") do_bench = true;
    else if (a == "-h" || a == "--help") return usage(), 0;
    else if (i == 1 && (a == "serve" || a == "stdio" || a == "status" || a == "install" || a == "uninstall"))
      command = a;
    else words.push_back(a);
  }
  if (limit == 0) limit = SIZE_MAX;
  if (char* real = realpath(root.c_str(), nullptr)) {
    root = real;
    std::free(real);
  }
  const std::string cache = cache_path(root);

  if (command == "serve") return serve_daemon(root, cache, threads, scan_threads);
  if (command == "install") return install(login);
  if (command == "uninstall") return uninstall();
  if (command == "status") {
    const int fd = connect_daemon(socket_path(cache));
    std::string resp;
    if (fd < 0 || !roundtrip(fd, "{\"op\":\"status\"}", resp)) {
      std::printf("{\"ok\":false,\"error\":\"no daemon is running for %s\"}\n", root.c_str());
      return 1;
    }
    std::printf("%s\n", resp.c_str());
    return 0;
  }
  if (command == "stdio") {
    const int fd = daemon_connection(root, cache, true, std::chrono::seconds(10));
    if (fd < 0) {
      std::fprintf(stderr, "cannot reach the daemon; see %s\n", log_path(cache).c_str());
      return 1;
    }
    return stdio(fd);
  }

  const bool query_mode = !words.empty() || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO);
  std::string text;
  for (const auto& w : words) text += (text.empty() ? "" : " ") + w;
  if (query_mode && !do_bench && !reindex && use_daemon) {
    // The daemon follows changes, so its answer is current.
    if (int fd = daemon_connection(root, cache, false, {}); fd >= 0) {
      int status = 1;
      if (print_daemon_results(fd, text, limit, status)) return status;
      close(fd);
    }
  }

  std::shared_ptr<const Index> ix;
  if (!reindex) {
    auto loaded = std::make_shared<Index>();
    auto t0 = Clock::now();
    if (load_index(*loaded, cache) && loaded->root == root) {
      ix = loaded;
      if (do_bench) std::fprintf(stderr, "loaded cache in %.0f ms\n", ms_since(t0));
    }
  }

  Engine engine(threads);
  SymbolIndex symbols;
  auto load_syms = [&]() -> const SymbolIndex* {
    return load_symbols(symbols, symbol_file(cache), *ix) ? &symbols : nullptr;
  };

  if (do_bench) {
    if (!ix) ix = build(root, cache, scan_threads);
    return bench(*ix, load_syms(), engine, words);
  }

  if (query_mode) {
    if (!ix && use_daemon) {
      // No index yet: the daemon builds it and answers once it has.
      if (int fd = daemon_connection(root, cache, true, std::chrono::seconds(10)); fd >= 0) {
        std::fprintf(stderr, "indexing the disk (once, about 20 s)...\n");
        for (;;) {
          int status = 1;
          if (print_daemon_results(fd, text, limit, status)) return status;
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
      }
    }
    if (!ix) ix = build(root, cache, scan_threads);
    const SymbolIndex* sx = load_syms();
    auto t0 = Clock::now();
    Query q = parse_query(text, home());
    Results r = run_query(*ix, sx, engine, q, q.limit ? q.limit : limit);
    double ms = ms_since(t0);
    if (!q.syms.empty() && !sx) std::fprintf(stderr, "no symbol index; run with --reindex\n");
    for (uint32_t e : r.top) {
      if (q.syms.empty()) {
        std::printf("%s\n", ix->path(e).c_str());
        continue;
      }
      const std::string path = ix->path(sx->occ[e]);
      const std::string_view name = sx->symbol(e);
      const SymbolLocation loc = locate_symbol(path, name);
      std::printf("%s:%u: %s %.*s\n", path.c_str(), loc.line, loc.found ? kind_name(loc.kind) : "?",
                  int(name.size()), name.data());
    }
    if (!q.error.empty()) std::fprintf(stderr, "%s\n", q.error.c_str());
    std::fprintf(stderr, "%llu matches in %.3f ms (%zu entries)\n", (unsigned long long)r.total, ms,
                 ix->count());
    std::fflush(stdout);
    // Later queries get current answers from the daemon.
    if (use_daemon && rescan) daemon_connection(root, cache, true, {});
    return r.total ? 0 : 1;
  }

  TuiOptions opt{root, cache, scan_threads, rescan};
  std::string chosen = run_tui(ix, engine, opt);
  if (chosen.empty()) return 1;
  std::printf("%s\n", chosen.c_str());
  return 0;
}
