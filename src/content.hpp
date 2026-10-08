#pragma once

// Content search: a trigram index over the text files in the home folder.
// Segments are immutable, memory-mapped files: a document table plus, per
// case-folded trigram, the documents containing it. A pattern becomes an
// AND/OR of trigrams; the posting lists pick candidate files, which are
// read fresh from disk and matched for real, so results are never stale.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"

namespace fplussearch {

class Pool;

// Files larger than this are not indexed.
constexpr uint64_t kMaxContentFile = uint64_t(1) << 20;

struct GrepOptions {
  std::string pattern;
  bool regex = false;  // false: literal
  size_t limit = 50;   // files with a match to return
  size_t per_file = 1;  // matching lines kept per file
  int budget_ms = 250;  // stop reading candidates after this; 0 = no budget
  std::string scope;    // in: real path prefix, empty = all of home
  std::function<bool(std::string_view path)> keep;  // optional extra file filter, may be null
};

struct LineMatch {
  uint32_t line;
  std::string text;
};

struct FileMatches {
  std::string path;
  std::vector<LineMatch> lines;
};

struct GrepResult {
  std::vector<FileMatches> files;
  size_t candidates = 0, read = 0;
  bool complete = true;  // false: the budget ran out before every candidate was read
  std::string error;
};

class ContentIndex {
 public:
  // dir: where segments live. home: the indexed area (a real path).
  ContentIndex(std::string dir, std::string home, unsigned readers = 12);
  ~ContentIndex();
  ContentIndex(const ContentIndex&) = delete;
  ContentIndex& operator=(const ContentIndex&) = delete;

  // Maps what's on disk; false if there is nothing usable yet.
  bool open();
  // Brings the index in line with the file index: (re)reads eligible files
  // whose path, size or mtime is new, drops the ones gone. `background`
  // lowers the reading threads' QoS.
  void sync(const Index& ix, bool background, ScanProgress* progress = nullptr);
  // Live updates, read straight from disk: relists `dirs` (direct children)
  // and rescans `trees` (whole subtrees). Cheap for a few folders.
  void update_paths(const std::vector<std::string>& dirs, const std::vector<std::string>& trees = {});

  GrepResult grep(const GrepOptions& o) const;
  // Same matching over files outside the index (e.g. in:/etc), read in order.
  GrepResult grep_files(const GrepOptions& o, const std::vector<std::string>& paths) const;
  // Whether `path` (a file or folder) is inside the indexed area.
  bool covers(std::string_view path) const;

  size_t docs() const;
  size_t bytes() const;

  struct Segment;
  struct State;

 private:
  std::shared_ptr<const State> state() const;
  void publish(std::shared_ptr<const State> s);
  // Indexes `docs` (sorted by path) into new segments; returns them.
  void add_docs(std::vector<struct DocIn>& docs, bool background);
  void maybe_merge();
  void save_manifest(const State& s) const;
  uint64_t next_id();

  std::string dir_, home_;
  mutable std::mutex m_;       // guards state_
  std::mutex write_m_;         // one writer at a time
  std::shared_ptr<const State> state_;
  std::atomic<uint64_t> next_id_{1};
  std::unique_ptr<Pool> readers_;  // grep's candidate reads
  mutable std::mutex read_m_;      // the reader pool runs one grep at a time
};

}  // namespace fplussearch
