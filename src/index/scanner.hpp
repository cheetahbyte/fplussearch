#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "index/cache_format.hpp"
#include "storage/chunked.hpp"
#include "storage/interner.hpp"
#include "storage/mapped_array.hpp"

namespace fplussearch::index_detail {

constexpr uint8_t kRawDir = 0xff;  // never a valid file kind byte
constexpr uint32_t kNoOld = UINT32_MAX;
constexpr uint64_t kNoMtime = UINT64_MAX;  // not a source file

struct CodeFile {
  uint32_t entry;
  uint64_t mtime;
};

struct Work {
  uint32_t id;
  std::string path;
  uint32_t old;  // the directory's entry in the previous index, or kNoOld
};

struct Item {
  uint32_t off;  // into the batch's name buffer
  uint16_t len;
  uint32_t hash;
  uint64_t size;
  uint64_t mtime;  // nanoseconds; only kept for source files
  uint8_t kind;
  bool code;
  uint32_t name = kNoOld;    // already interned (copied from the previous index)
  uint32_t parent = kNoOld;  // item in the same batch; kNoOld = the listed directory
  uint8_t flags = 0;         // kFlag*
};

struct Subdir {
  uint32_t item;
  std::string path;
  uint32_t old;
};

class Scanner {
 public:
  // With `old`, directories not named in `changes` (and not new) are copied
  // from it instead of read from disk.
  Scanner(std::string root, ScanProgress* progress, const Index* old = nullptr, const Changes* changes = nullptr);
  Built run(unsigned threads, bool background);

 private:
  void add_entry(uint32_t name, uint32_t parent, uint64_t size, uint8_t kind, uint64_t mtime_ns, uint8_t flags);
  bool take(unsigned self, Work& w);
  void worker(unsigned self);
  void index_old();
  uint64_t old_code_mtime(uint32_t e) const;
  Item copy_item(uint32_t c, uint32_t parent) const;
  void copy_dir(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs);
  void copy_tree(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs);
  void read_dir(const Work& w, std::vector<char>& buf, std::vector<char>& names, std::vector<Item>& items,
                std::vector<Subdir>& subdirs);
  void publish(unsigned self, const Work& w, const std::vector<char>& names, const std::vector<Item>& items,
               std::vector<Subdir>& subdirs);
  void sort_names(BigVec<uint32_t>& ids) const;
  void lay_out_dirs(Built& b, const BigVec<uint32_t>& new_id, uint32_t dirs);
  Built finish();

  std::string root_;
  ScanProgress* progress_;
  std::vector<std::string> excluded_;
  const Index* old_;
  std::unordered_set<std::string> dirty_, trees_;
  std::unordered_set<std::string> above_;  // directories with changes below them
  BigVec<uint32_t> old_name_;         // previous entry -> interned name
  BigVec<uint32_t> old_child_start_;  // previous directory -> range in old_children_
  BigVec<uint32_t> old_children_;
  struct alignas(128) Queue {
    std::mutex m;
    std::deque<Work> q;
  };
  std::mutex m_;  // the entry arrays and names_
  std::vector<std::unique_ptr<Queue>> queues_;
  std::atomic<size_t> pending_{0};  // directories queued or being read

  Interner names_;
  Chunked<uint32_t> name_, parent_, size_, mtime_;
  Chunked<uint8_t> kind_, flags_;
  BigVec<Overflow> big_;
  BigVec<CodeFile> code_;  // raw entry ids until finish()
};

}  // namespace fplussearch::index_detail
