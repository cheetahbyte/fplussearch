#include "../src/content.cpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <filesystem>
#include <random>
#include <cassert>
#include <string>

using namespace fplussearch;

static void write_file(const std::string& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}

static bool has(const GrepResult& r, const std::string& p) {
  for (const auto& f : r.files)
    if (f.path == p) return true;
  return false;
}

static std::vector<std::string> paths(const GrepResult& r) {
  std::vector<std::string> out;
  for (const auto& f : r.files) out.push_back(f.path);
  return out;
}

static void set_mtime(const std::string& p, time_t seconds) {
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = seconds;
  assert(utimensat(AT_FDCWD, p.c_str(), times, 0) == 0);
}

int main() {
  std::mt19937 random(713);
  for (size_t length : {size_t(0), size_t(4), size_t(7), size_t(100), size_t(10000)}) {
    std::string text(length, ' ');
    for (char& c : text) c = char(random() % 256);
    Bloom b;
    make_bloom(text, b);
    for (size_t n : {size_t(5), size_t(7)})
      for (size_t i = 0; i + n <= text.size(); ++i)
        assert(bloom_has(n == 5 ? b.short_bits : b.long_bits, gram_hash(text.data() + i, n)));
  }
  char root[] = "/tmp/fplus-content-XXXXXX";
  if (!mkdtemp(root)) return EXIT_FAILURE;
  const std::string home = root, cache = home + "/cache";
  struct Cleanup { std::string path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{home};
  // Keep the common "est" posting sparse and larger than the skip threshold.
  for (int i = 0; i < 22000; ++i)
    write_file(home + "/doc-" + std::to_string(i) + ".txt", i < 2500 ? "test\n" : "opaque\n");
  const std::string hit = home + "/hit.txt", miss = home + "/miss.txt";
  write_file(hit, "prefix qz7tesy suffix\n");
  write_file(miss, "qz7t z7te 7tes tesy\n");

  auto ix = std::make_unique<ContentIndex>(cache, home, 2);
  ix->open();
  ix->update_paths({}, {home});
  GrepOptions q;
  q.pattern = "qz7tesy";
  q.budget_ms = 0;
  const auto literal = ix->grep(q);
  assert(literal.read == 1 && has(literal, hit));
  q.pattern = "qz7";
  if (!has(ix->grep(q), hit)) { std::cerr << "short failed\n"; return EXIT_FAILURE; }
  q.pattern = "qz7tes.";
  q.regex = true;
  if (!has(ix->grep(q), hit)) { std::cerr << "regex failed\n"; return EXIT_FAILURE; }

  // Same-size edits with unchanged mtime must refresh the Bloom filter.
  q.regex = false;
  q.pattern = "qz7test";
  assert(ix->grep(q).files.empty());
  struct stat before{};
  assert(stat(hit.c_str(), &before) == 0);
  write_file(hit, "prefix qz7test suffix\n");
  struct timespec times[2]{};
  times[0].tv_nsec = UTIME_OMIT;
  times[1] = before.st_mtimespec;
  // Preserve mtime too: ctime must still reveal the edit.
  assert(utimensat(AT_FDCWD, hit.c_str(), times, 0) == 0);
  assert(ix->grep(q).files.empty());
  ix->update_paths({home});
  if (!has(ix->grep(q), hit)) { std::cerr << "refresh failed\n"; return EXIT_FAILURE; }

  // Reopen preserves the sidecar and remains searchable.
  ix.reset();
  ix = std::make_unique<ContentIndex>(cache, home, 2);
  ix->open();
  if (!has(ix->grep(q), hit)) { std::cerr << "reopen failed\n"; return EXIT_FAILURE; }

  q.pattern = "QZ7TEST";
  assert(!has(ix->grep(q), hit));
  q.pattern = "qz7test";

  // Removing the optional sidecar exercises legacy migration on update.
  for (const auto& e : {"cache/seg-000001.blm", "cache/seg-000002.blm", "cache/seg-000003.blm"}) unlink((home + "/" + e).c_str());
  ix.reset();
  ix = std::make_unique<ContentIndex>(cache, home, 2);
  ix->open();
  ix->update_paths({}, {home});
  if (!has(ix->grep(q), hit)) { std::cerr << "legacy failed\n"; return EXIT_FAILURE; }
  // A Bloom-positive file is still read fresh before any match is returned.
  write_file(hit, "the indexed match was removed\n");
  const auto removed = ix->grep(q);
  assert(removed.read > 0 && !has(removed, hit));

  // Candidate ordering stays exact when matching files are spread across
  // several live segments. This also covers dead docs and ties in rank and
  // mtime, where path order is the final tiebreaker.
  const std::string ordered_home = home + "/ordering-home";
  const std::string cache2 = ordered_home + "/cache";
  const std::string segment_a = ordered_home + "/a", segment_b = ordered_home + "/b",
                    segment_c = ordered_home + "/c", segment_d = ordered_home + "/d";
  assert(mkdir(ordered_home.c_str(), 0755) == 0);
  assert(mkdir(segment_a.c_str(), 0755) == 0);
  assert(mkdir(segment_b.c_str(), 0755) == 0);
  assert(mkdir(segment_c.c_str(), 0755) == 0);
  assert(mkdir(segment_d.c_str(), 0755) == 0);
  const std::string alpha = segment_a + "/alpha.txt", beta = segment_b + "/beta.txt",
                    gamma = segment_c + "/gamma.txt", old = segment_d + "/old.txt",
                    log = segment_d + "/zeta.log";
  for (const auto& p : {alpha, beta, gamma, old, log}) write_file(p, "ordering needle\n");
  set_mtime(alpha, 100);
  set_mtime(beta, 200);
  set_mtime(gamma, 200);
  set_mtime(old, 50);
  set_mtime(log, 300);

  auto ordered = std::make_unique<ContentIndex>(cache2, ordered_home, 2);
  ordered->open();
  ordered->update_paths({segment_a});
  ordered->update_paths({segment_b});
  ordered->update_paths({segment_c});
  ordered->update_paths({segment_d});
  GrepOptions ordering_query;
  ordering_query.pattern = "ordering needle";
  ordering_query.budget_ms = 0;
  ordering_query.limit = 20;
  assert(paths(ordered->grep(ordering_query)) == std::vector<std::string>({beta, gamma, alpha, old, log}));

  // A subsequent direct update kills the old document in its segment; the
  // remaining live docs keep the same cross-segment order.
  write_file(old, "gone\n");
  ordered->update_paths({segment_d});
  assert(paths(ordered->grep(ordering_query)) == std::vector<std::string>({beta, gamma, alpha, log}));
  std::cout << "Content Bloom regression checks passed\n";
}
