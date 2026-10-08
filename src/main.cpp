#include <mach/mach.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "index.hpp"
#include "search.hpp"
#include "tui.hpp"

using namespace fplussearch;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void usage() {
  std::fprintf(stderr,
               "usage: fplussearch [options] [query]\n"
               "\n"
               "  no query       interactive search; Enter prints the selected path\n"
               "  query          print matching paths and exit\n"
               "\n"
               "  --root PATH    directory to index (default /)\n"
               "  --reindex      ignore the cached index and rescan first\n"
               "  --no-rescan    use the cached index without refreshing it\n"
               "  --threads N    search threads (default: all cores)\n"
               "  --scan-threads N  indexing threads (default: 8)\n"
               "  -n N           max results to print (default 50, 0 = all)\n"
               "  --bench        time each query (or a default set) over many runs\n"
               "\n"
               "query syntax: words (substring of name, case-insensitive, all must match)\n"
               "  \"quoted phrase\"  size:>1gb size:<=10mb  type:video|audio|image|doc|archive|code|dir|file\n"
               "  ext:mp4,mov\n");
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

int bench(const Index& ix, Engine& engine, std::vector<std::string> queries) {
  if (queries.empty())
    queries = {"size:>1gb type:video", "a", "e", "readme", "config.json", "ext:mp4", "type:image",
               "lib", "node_modules", "zzqxj", "png ico", "size:>100mb"};
  std::printf("%zu entries (%u directories), index %.1f MB\n", ix.count(), ix.dirs, ix.bytes / 1e6);
  // hot: back to back. typing: 80 ms apart, like keystrokes. cold: 500 ms
  // apart, after the workers have gone to sleep and the cores have idled.
  std::printf("%-24s %10s %10s %10s %10s\n", "query", "matches", "hot ms", "typing ms", "cold ms");
  auto timed = [&](const std::string& qs, Results& r) {
    auto t0 = Clock::now();
    Query q = parse_query(qs);
    r = engine.search(ix, q, 50);
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
  bool reindex = false, rescan = true, do_bench = false;
  unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  unsigned scan_threads = std::min(8u, threads);  // APFS stops scaling around here
  size_t limit = 50;
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
    else if (a == "--threads") threads = unsigned(std::max(1, std::atoi(value().c_str())));
    else if (a == "--scan-threads") scan_threads = unsigned(std::max(1, std::atoi(value().c_str())));
    else if (a == "-n") limit = size_t(std::atoll(value().c_str()));
    else if (a == "--bench") do_bench = true;
    else if (a == "-h" || a == "--help") return usage(), 0;
    else words.push_back(a);
  }
  if (limit == 0) limit = SIZE_MAX;
  if (char* real = realpath(root.c_str(), nullptr)) {
    root = real;
    std::free(real);
  }

  const std::string cache = cache_path(root);
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

  if (do_bench) {
    if (!ix) ix = build(root, cache, scan_threads);
    return bench(*ix, engine, words);
  }

  if (!words.empty() || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
    if (!ix) ix = build(root, cache, scan_threads);
    std::string text;
    for (const auto& w : words) text += (text.empty() ? "" : " ") + w;
    auto t0 = Clock::now();
    Query q = parse_query(text);
    Results r = engine.search(*ix, q, limit);
    double ms = ms_since(t0);
    for (uint32_t e : r.top) std::printf("%s\n", ix->path(e).c_str());
    if (!q.error.empty()) std::fprintf(stderr, "%s\n", q.error.c_str());
    std::fprintf(stderr, "%llu matches in %.3f ms (%zu entries)\n", (unsigned long long)r.total, ms,
                 ix->count());
    return r.total ? 0 : 1;
  }

  TuiOptions opt{root, cache, scan_threads, rescan};
  std::string chosen = run_tui(ix, engine, opt);
  if (chosen.empty()) return 1;
  std::printf("%s\n", chosen.c_str());
  return 0;
}
