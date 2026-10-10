#include "cli/benchmark.hpp"
#include "cli/daemon.hpp"
#include "cli/installation.hpp"
#include "cli/runtime.hpp"
#include "cli/usage.hpp"
#include "daemon/socket.hpp"
#include "index/index.hpp"
#include "search/engine.hpp"
#include "symbols/symbol_index.hpp"

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace fplussearch;
using namespace fplussearch::cli;

namespace {

std::shared_ptr<const Index> build(const std::string& root, const std::string& cache, unsigned threads) {
  auto t0 = Clock::now();
  auto ix = std::make_shared<Index>(build_index(root, threads, nullptr, false, cache));
  std::fprintf(stderr, "indexed %zu entries in %.2f s\n", ix->count(), ms_since(t0) / 1000);
  return ix;
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
  if (command.empty() && words.empty() && !do_bench) return usage(), 0;
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

  std::string text;
  for (const auto& w : words) text += (text.empty() ? "" : " ") + w;
  if (!do_bench && !reindex && use_daemon) {
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
