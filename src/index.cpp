#include "index.hpp"

#include "live.hpp"
#include "store.hpp"
#include "symbols.hpp"
#include "symindex.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fplussearch {

namespace {

const std::unordered_map<std::string_view, uint8_t>& ext_table() {
  static const std::unordered_map<std::string_view, uint8_t> table = [] {
    std::unordered_map<std::string_view, uint8_t> m;
    auto add = [&](uint8_t c, std::initializer_list<std::string_view> xs) {
      for (auto x : xs) m.emplace(x, c);
    };
    add(CatVideo, {"mp4", "m4v", "mov", "mkv", "avi", "webm", "wmv", "flv", "mpg", "mpeg", "3gp",
                   "mts", "m2ts", "vob", "ogv", "hevc"});
    add(CatAudio, {"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus", "aif", "aiff", "alac", "wma",
                   "mid", "midi", "caf"});
    add(CatImage, {"jpg", "jpeg", "png", "gif", "heic", "heif", "webp", "tif", "tiff", "bmp",
                   "svg", "ico", "icns", "raw", "cr2", "cr3", "nef", "arw", "dng", "psd", "avif"});
    add(CatDoc, {"pdf", "doc", "docx", "txt", "md", "rtf", "pages", "odt", "xls", "xlsx", "csv",
                 "ppt", "pptx", "key", "numbers", "epub", "tex"});
    add(CatArchive, {"zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar", "dmg", "iso", "zst",
                     "pkg", "xip"});
    add(CatCode, {"c",  "cc",   "cpp",  "cxx", "h",    "hh",   "hpp",  "rs",   "go",
                  "py", "js",   "mjs",  "ts",  "tsx",  "jsx",  "java", "kt",   "swift",
                  "m",  "mm",   "rb",   "php", "sh",   "fish", "zsh",  "lua",  "cs",
                  "zig", "json", "toml", "yaml", "yml", "html", "css",  "scss", "sql"});
    add(CatFont, {"ttf", "otf", "woff", "woff2", "ttc", "dfont"});
    return m;
  }();
  return table;
}

// Field order in the cache file.
enum Field : size_t {
  kRoot = 0,
  kSections = 1,  // kSectionFields per section
  kParents = kSections + 4 * kSectionFields,
  kFileKind,
  kFileSize,
  kBigSizes,
  kCodeEntry,
  kCodeMtime,
  kMtime,
  kFlags,
  kDirPrior,
  kDirOrder,
  kDirPre,
  kDirEnd,
  kChildStart,
  kChildren,
  kFields
};
enum Meta : size_t { kMetaN, kMetaDirs, kMetaParentBits, kMetaBuildId, kMetaEventId };

constexpr char kMagic[8] = {'F', 'S', 'R', 'C', 'H', 'J', 'X', '1'};
constexpr uint8_t kRawDir = 0xff;  // never a valid file kind byte
constexpr uint32_t kNoOld = UINT32_MAX;
constexpr uint64_t kNoMtime = UINT64_MAX;  // not a source file

struct Built {
  std::array<uint64_t, 8> meta{};
  std::vector<Blob> fields = std::vector<Blob>(kFields);
};

struct CodeFile {
  uint32_t entry;
  uint64_t mtime;
};

bool attach(Index& ix, Fields&& f) {
  Span<char> root;
  if (!f.get(kRoot, root)) return false;
  Index r;
  r.root.assign(root.p, root.n);
  r.n = uint32_t(f.meta[kMetaN]);
  r.dirs = uint32_t(f.meta[kMetaDirs]);
  r.parent_bits = uint32_t(f.meta[kMetaParentBits]);
  r.build_id = f.meta[kMetaBuildId];
  r.event_id = f.meta[kMetaEventId];
  bool ok = f.meta[kMetaN] <= UINT32_MAX && r.dirs <= r.n;
  for (size_t s = 0; s < 4; ++s) ok = ok && f.section(kSections + s * kSectionFields, r.sections[s], s % 2 == 1);
  ok = ok && f.get(kParents, r.parents) && f.get(kFileKind, r.file_kind) && f.get(kFileSize, r.file_size) &&
       f.get(kBigSizes, r.big_sizes) &&
       f.get(kCodeEntry, r.code_entry) && f.get(kCodeMtime, r.code_mtime) && r.code_entry.n == r.code_mtime.n &&
       f.get(kMtime, r.mtime) && f.get(kFlags, r.flags) && f.get(kDirPrior, r.dir_prior) &&
       f.get(kDirOrder, r.dir_order) && f.get(kDirPre, r.dir_pre) && f.get(kDirEnd, r.dir_end) &&
       r.mtime.n == r.n && r.flags.n == r.n && r.dir_prior.n == r.dirs && r.dir_order.n == r.dirs &&
       r.dir_pre.n == r.dirs && r.dir_end.n == r.dirs && f.get(kChildStart, r.child_start) &&
       f.get(kChildren, r.children) && r.child_start.n == size_t(r.dirs) + 1 && r.children.n + 1 == r.n &&
       r.file_kind.n == r.n - r.dirs && r.file_size.n == r.n - r.dirs && r.parent_bits >= 1 &&
       r.parent_bits <= 32 && r.parents.n >= (uint64_t(r.n) * r.parent_bits + 7) / 8 + 8;
  if (!ok) return false;
  r.bytes = f.bytes;
  r.backing = std::move(f.backing);
  for (auto& s : r.sections) {
    auto planes = std::make_shared<std::vector<std::array<uint64_t, kBlock>>>(
        (size_t(s.names()) + kBlock - 1) / kBlock);
    for (uint32_t i = 0; i < s.names(); ++i) {
      uint64_t mask = s.mask[i];
      while (mask) {
        (*planes)[i / kBlock][std::countr_zero(mask)] |= uint64_t(1) << (i % kBlock);
        mask &= mask - 1;
      }
    }
    s.mask_planes = std::move(planes);
  }
  ix = std::move(r);
  return true;
}

// How much a directory's name moves everything under it in ranking. Depth 1
// is a top-level folder of the whole disk.
int prior_adjust(std::string_view name, int depth, bool whole_disk) {
  if (whole_disk && depth == 1) {
    if (name == "Users") return 0;
    if (name == "Applications") return 10;
    if (name == "Volumes") return -10;
    if (name == "Library" || name == "opt") return -25;
    if (name == "System") return -40;
    return -35;
  }
  if (name == "Applications") return 30;
  if (name.ends_with(".app")) return -25;
  static constexpr std::string_view kBundles[] = {
      ".framework", ".bundle", ".plugin", ".appex", ".kext", ".xpc", ".lproj", ".xcassets", ".photoslibrary",
      ".musiclibrary", ".tvlibrary", ".imovielibrary", ".dsym", ".xcarchive", ".sdk", ".platform"};
  for (std::string_view x : kBundles) {
    if (name.size() <= x.size()) continue;
    bool eq = true;
    for (size_t i = 0; i < x.size() && eq; ++i) eq = kFold[uint8_t(name[name.size() - x.size() + i])] == uint8_t(x[i]);
    if (eq) return -20;
  }
  if (name.starts_with('.')) return -25;
  if (name == "Library") return -20;
  for (std::string_view x : {"Caches", "caches", "cache", "Cache", "Logs", "DerivedData", "CoreSimulator"})
    if (name == x) return -20;
  for (std::string_view x : {"node_modules", "__pycache__", "site-packages", "Pods", "venv", "bower_components"})
    if (name == x) return -30;
  for (std::string_view x : {"target", "build", "dist", "out", "vendor", "deps", "tmp", "temp"})
    if (name == x) return -12;
  for (std::string_view x : {"folders", "Containers", "Group Containers"})
    if (name == x) return -10;
  if (name == "Application Support") return -5;
  return 0;
}

uint64_t new_build_id() {
  return hash_bytes(uint64_t(std::chrono::system_clock::now().time_since_epoch().count()),
                    std::to_string(std::random_device{}()));
}

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

constexpr uint32_t kUnknownEntries = UINT32_MAX;

// Calls on(name, vnode type, size, mtime in ns, BSD flags, entries) for each
// entry of the open directory `fd`; `entries` is a subdirectory's entry
// count (kUnknownEntries for files and mount points). One
// getattrlistbulk(2) call returns hundreds of entries with their
// attributes, so there is no stat per file.
template <typename On>
void read_listing(int fd, std::vector<char>& buf, On&& on) {
  attrlist al{};
  al.bitmapcount = ATTR_BIT_MAP_COUNT;
  al.commonattr =
      ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FLAGS;
  al.dirattr = ATTR_DIR_ENTRYCOUNT | ATTR_DIR_MOUNTSTATUS;
  al.fileattr = ATTR_FILE_DATALENGTH;
  for (;;) {
    const int n = getattrlistbulk(fd, &al, buf.data(), buf.size(), 0);
    if (n <= 0) break;
    const char* p = buf.data();
    for (int k = 0; k < n; ++k) {
      const char* entry = p;
      uint32_t len;
      std::memcpy(&len, entry, sizeof len);
      p += len;
      // Layout per getattrlistbulk(2): length, returned attrs, error, then
      // requested attributes in bit order.
      const char* f = entry + sizeof len;
      attribute_set_t ret;
      std::memcpy(&ret, f, sizeof ret);
      f += sizeof ret;
      if (ret.commonattr & ATTR_CMN_ERROR) f += sizeof(uint32_t);
      if (!(ret.commonattr & ATTR_CMN_NAME)) continue;
      attrreference_t ref;
      std::memcpy(&ref, f, sizeof ref);
      const char* nm = f + ref.attr_dataoffset;
      const size_t nl = strnlen(nm, ref.attr_length);
      f += sizeof ref;
      uint32_t type = VNON;
      if (ret.commonattr & ATTR_CMN_OBJTYPE) {
        std::memcpy(&type, f, sizeof type);
        f += sizeof type;
      }
      uint64_t mtime = 0;
      if (ret.commonattr & ATTR_CMN_MODTIME) {
        timespec ts;
        std::memcpy(&ts, f, sizeof ts);
        f += sizeof ts;
        mtime = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
      }
      uint32_t bsd_flags = 0;
      if (ret.commonattr & ATTR_CMN_FLAGS) {
        std::memcpy(&bsd_flags, f, sizeof bsd_flags);
        f += sizeof bsd_flags;
      }
      uint32_t entries = kUnknownEntries;
      if (ret.dirattr & ATTR_DIR_ENTRYCOUNT) {
        std::memcpy(&entries, f, sizeof entries);
        f += sizeof entries;
      }
      if (ret.dirattr & ATTR_DIR_MOUNTSTATUS) {
        uint32_t mount;
        std::memcpy(&mount, f, sizeof mount);
        f += sizeof mount;
        if (mount & (DIR_MNTSTATUS_MNTPOINT | DIR_MNTSTATUS_TRIGGER)) entries = kUnknownEntries;
      }
      uint64_t size = 0;
      if (ret.fileattr & ATTR_FILE_DATALENGTH) {
        off_t v;
        std::memcpy(&v, f, sizeof v);
        size = uint64_t(v);
      }
      if (nl == 0 || nl > 1023) continue;
      on(std::string_view(nm, nl), type, size, mtime, bsd_flags, entries);
    }
  }
}

uint8_t entry_flags(uint32_t type, uint32_t bsd_flags) {
  return uint8_t((bsd_flags & UF_HIDDEN ? kFlagHidden : 0) | (type == VLNK ? kFlagLink : 0));
}

// Runs fn(0) .. fn(n - 1) on up to 8 threads.
template <typename Fn>
void parallel(size_t n, Fn&& fn) {
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < n;) fn(i);
  };
  std::vector<std::thread> ts;
  for (size_t t = 1; t < std::min<size_t>(n, 8); ++t) ts.emplace_back(work);
  work();
  for (auto& t : ts) t.join();
}

std::string join(const std::string& dir, std::string_view name) {
  std::string s;
  s.reserve(dir.size() + name.size() + 1);
  s = dir;
  if (dir != "/") s += '/';
  s += name;
  return s;
}

class Scanner {
 public:
  // With `old`, directories not named in `changes` (and not new) are copied
  // from it instead of read from disk.
  Scanner(std::string root, ScanProgress* progress, const Index* old = nullptr, const Changes* changes = nullptr)
      : root_(std::move(root)), progress_(progress), excluded_(excluded_dirs(root_)), old_(old) {
    if (changes) {
      dirty_.insert(changes->dirs.begin(), changes->dirs.end());
      trees_.insert(changes->trees.begin(), changes->trees.end());
      for (const auto* set : {&changes->dirs, &changes->trees})
        for (const std::string& p : *set)
          for (size_t i = p.find('/', 1); i != std::string::npos; i = p.find('/', i + 1)) above_.insert(p.substr(0, i));
      above_.insert(root_);
    }
  }

  Built run(unsigned threads, bool background) {
    add_entry(names_.intern("", 0), 0, 0, kRawDir, 0, 0);
    if (old_) index_old();
    for (unsigned i = 0; i < threads; ++i) queues_.push_back(std::make_unique<Queue>());
    queues_[0]->q.push_back({0, root_, old_ ? 0 : kNoOld});
    pending_ = 1;

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i)
      pool.emplace_back([this, background, i] {
        // In the foreground someone waits for the index: keep the scan on
        // the performance cores.
        pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
        // Listing a folder of dataless (cloud placeholder) files must not
        // download them or wait on their file provider.
        setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
        worker(i);
      });
    for (auto& t : pool) t.join();
    return finish();
  }

 private:
  void add_entry(uint32_t name, uint32_t parent, uint64_t size, uint8_t kind, uint64_t mtime_ns, uint8_t flags) {
    if (kind != kRawDir && size >= UINT32_MAX) big_.push_back({uint32_t(name_.size()), 0, size});
    name_.push_back(name);
    parent_.push_back(parent);
    size_.push_back(uint32_t(std::min<uint64_t>(size, UINT32_MAX)));
    kind_.push_back(kind);
    mtime_.push_back(uint32_t(std::min<uint64_t>(mtime_ns / 1000000000, UINT32_MAX)));
    flags_.push_back(flags);
  }

  // Takes the newest directory from this thread's queue, or steals the
  // oldest from another's: threads stay in separate subtrees instead of
  // contending on the same parent directory in the kernel.
  bool take(unsigned self, Work& w) {
    const size_t n = queues_.size();
    for (size_t k = 0; k < n; ++k) {
      Queue& qu = *queues_[(self + k) % n];
      std::lock_guard lk(qu.m);
      if (qu.q.empty()) continue;
      if (k == 0) {
        w = std::move(qu.q.back());
        qu.q.pop_back();
      } else {
        w = std::move(qu.q.front());
        qu.q.pop_front();
      }
      return true;
    }
    return false;
  }

  void worker(unsigned self) {
    std::vector<char> buf(256 * 1024);
    std::vector<char> names;
    std::vector<Item> items;
    std::vector<Subdir> subdirs;
    for (unsigned idle = 0;;) {
      Work w;
      if (!take(self, w)) {
        if (pending_.load(std::memory_order_acquire) == 0) return;
        if (++idle < 64) std::this_thread::yield();
        else std::this_thread::sleep_for(std::chrono::microseconds(100));
        continue;
      }
      idle = 0;
      if (w.old != kNoOld && !dirty_.contains(w.path) && !trees_.contains(w.path)) {
        if (above_.contains(w.path)) copy_dir(w, items, subdirs);
        else copy_tree(w, items, subdirs);
      } else {
        read_dir(w, buf, names, items, subdirs);
      }
      publish(self, w, names, items, subdirs);
    }
  }

  // Builds what copy_dir and read_dir need from the previous index: interned
  // names per entry, children per directory, and source file mtimes.
  void index_old() {
    const Index& o = *old_;
    old_name_.assign(o.n, 0);
    std::array<char, 256 + 32> buf;
    for (const Section& sec : o.sections) {
      for (uint32_t b = 0; b < sec.blocks(); ++b) {
        uint32_t e = sec.block_first[b];
        const char* bs = sec.bytes.p + sec.block_off[b];
        for (uint32_t k = b * kBlock, end = std::min(sec.names(), k + kBlock); k < end; ++k) {
          std::string_view nm;
          if (sec.hex) {
            size_t len;
            unpack_hex(bs + sec.name_off[k], buf.data(), len);
            nm = {buf.data(), len};
          } else {
            nm = bs + sec.name_off[k];
          }
          const uint32_t id = names_.intern(nm, uint32_t(std::hash<std::string_view>{}(nm)));
          for (uint32_t c = sec.count(k); c--;) old_name_[e++] = id;
        }
      }
    }
    old_child_start_.assign(size_t(o.dirs) + 1, 0);
    for (uint32_t e = 1; e < o.n; ++e) ++old_child_start_[o.parent(e) + 1];
    for (uint32_t d = 0; d < o.dirs; ++d) old_child_start_[d + 1] += old_child_start_[d];
    old_children_.resize(o.n);
    BigVec<uint32_t> pos(old_child_start_.begin(), old_child_start_.end() - 1);
    for (uint32_t e = 1; e < o.n; ++e) old_children_[pos[o.parent(e)]++] = e;
  }

  uint64_t old_code_mtime(uint32_t e) const {
    const Index& o = *old_;
    const uint32_t* it = std::lower_bound(o.code_entry.p, o.code_entry.p + o.code_entry.n, e);
    return it != o.code_entry.p + o.code_entry.n && *it == e ? o.code_mtime[size_t(it - o.code_entry.p)] : kNoMtime;
  }

  // Copies old entry `c` as an item listed under batch item `parent`.
  Item copy_item(uint32_t c, uint32_t parent) const {
    const Index& o = *old_;
    const uint64_t seconds = uint64_t(o.mtime[c]) * 1000000000;
    if (o.is_dir(c)) return {0, 0, 0, 0, seconds, kRawDir, false, old_name_[c], parent, o.flags[c]};
    const uint64_t code = old_code_mtime(c);
    return {0, 0, 0, o.size(c), code != kNoMtime ? code : seconds, o.file_kind[c - o.dirs], code != kNoMtime,
            old_name_[c], parent, o.flags[c]};
  }

  // Lists an unchanged directory with changes below it from the previous index.
  void copy_dir(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs) {
    items.clear();
    subdirs.clear();
    for (uint32_t i = old_child_start_[w.old]; i < old_child_start_[w.old + 1]; ++i) {
      const uint32_t c = old_children_[i];
      items.push_back(copy_item(c, kNoOld));
      if (old_->is_dir(c)) subdirs.push_back({uint32_t(items.size() - 1), join(w.path, old_->name(c)), c});
    }
  }

  // Copies a subtree without changes from the previous index in one batch.
  void copy_tree(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs) {
    items.clear();
    subdirs.clear();
    std::vector<std::pair<uint32_t, uint32_t>> stack{{w.old, kNoOld}};  // old directory, its item
    while (!stack.empty()) {
      const auto [d, item] = stack.back();
      stack.pop_back();
      for (uint32_t i = old_child_start_[d]; i < old_child_start_[d + 1]; ++i) {
        const uint32_t c = old_children_[i];
        items.push_back(copy_item(c, item));
        if (old_->is_dir(c)) stack.push_back({c, uint32_t(items.size() - 1)});
      }
    }
  }

  void read_dir(const Work& w, std::vector<char>& buf, std::vector<char>& names, std::vector<Item>& items,
                std::vector<Subdir>& subdirs) {
    names.clear();
    items.clear();
    subdirs.clear();
    int fd = open(w.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    // A changed directory keeps the previous index's copy of subdirectories
    // that still exist, unless its whole subtree changed.
    std::unordered_map<std::string, uint32_t> old_dirs;
    if (w.old != kNoOld && !trees_.contains(w.path)) {
      for (uint32_t i = old_child_start_[w.old]; i < old_child_start_[w.old + 1]; ++i) {
        const uint32_t c = old_children_[i];
        if (old_->is_dir(c)) old_dirs.emplace(old_->name(c), c);
      }
    }

    read_listing(fd, buf, [&](std::string_view name, uint32_t type, uint64_t size, uint64_t mtime, uint32_t bsd_flags,
                              uint32_t) {
      const bool dir = type == VDIR;
      const uint8_t kind = dir ? kRawDir : uint8_t(size_class(size) << 4 | categorize(name));
      const bool code = !dir && type == VREG && size <= kMaxSourceSize && lang_of(name) != Lang::None;
      items.push_back({uint32_t(names.size()), uint16_t(name.size()), uint32_t(std::hash<std::string_view>{}(name)),
                       size, mtime, kind, code, kNoOld, kNoOld, entry_flags(type, bsd_flags)});
      names.insert(names.end(), name.begin(), name.end());
      if (dir) {
        std::string child = join(w.path, name);
        bool skip = false;
        for (const auto& ex : excluded_) skip |= child == ex;
        if (!skip) {
          const auto it = old_dirs.find(std::string(name));
          const uint32_t old = it == old_dirs.end() ? kNoOld : it->second;
          subdirs.push_back({uint32_t(items.size() - 1), std::move(child), old});
        }
      }
    });
    close(fd);
  }

  void publish(unsigned self, const Work& w, const std::vector<char>& names, const std::vector<Item>& items,
               std::vector<Subdir>& subdirs) {
    uint32_t base;
    {
      std::lock_guard lk(m_);
      base = uint32_t(name_.size());
      if (base + items.size() >= UINT32_MAX) {
        subdirs.clear();
      } else {
        for (const auto& it : items) {
          if (it.code) code_.push_back({uint32_t(name_.size()), it.mtime});
          const uint32_t name = it.name != kNoOld ? it.name : names_.intern({names.data() + it.off, it.len}, it.hash);
          add_entry(name, it.parent == kNoOld ? w.id : base + it.parent, it.size, it.kind, it.mtime, it.flags);
        }
        if (progress_) progress_->entries.store(name_.size(), std::memory_order_relaxed);
      }
    }
    if (!subdirs.empty()) {
      pending_.fetch_add(subdirs.size(), std::memory_order_relaxed);
      Queue& qu = *queues_[self];
      std::lock_guard lk(qu.m);
      for (auto& sd : subdirs) qu.q.push_back({base + sd.item, std::move(sd.path), sd.old});
    }
    pending_.fetch_sub(1, std::memory_order_release);
  }

  // Sorts name ids by name: bucketed by first byte, buckets sorted in parallel.
  void sort_names(BigVec<uint32_t>& ids) const {
    std::array<std::vector<uint32_t>, 256> bucket;
    for (uint32_t u : ids) {
      const std::string_view nm = names_.get(u);
      bucket[nm.empty() ? 0 : uint8_t(nm[0])].push_back(u);
    }
    parallel(256, [&](size_t i) {
      std::sort(bucket[i].begin(), bucket[i].end(), [&](uint32_t a, uint32_t c) { return names_.get(a) < names_.get(c); });
    });
    size_t at = 0;
    for (const auto& bk : bucket)
      for (uint32_t u : bk) ids[at++] = u;
  }

  // Directory order and priors: dir_order lists directories depth-first so
  // each subtree is a range, and a directory's prior adds up how wanted
  // its own name and its ancestors' names make what's under it.
  void lay_out_dirs(Built& b, const BigVec<uint32_t>& new_id, uint32_t dirs) {
    BigVec<uint32_t> raw(dirs), par(dirs, 0);  // new directory id -> raw entry, new parent
    for (size_t e = 0; e < name_.size(); ++e)
      if (kind_[e] == kRawDir) raw[new_id[e]] = uint32_t(e), par[new_id[e]] = new_id[parent_[e]];
    BigVec<uint32_t> start(size_t(dirs) + 1, 0), kids(dirs);
    for (uint32_t d = 1; d < dirs; ++d) ++start[par[d] + 1];
    for (uint32_t d = 0; d < dirs; ++d) start[d + 1] += start[d];
    {
      BigVec<uint32_t> pos(start.begin(), start.end() - 1);
      for (uint32_t d = 1; d < dirs; ++d) kids[pos[par[d]]++] = d;
    }
    // Home's components below the root: entering home raises everything in it.
    std::vector<std::string> home;
    if (const char* h = std::getenv("HOME"); h && std::string_view(h).starts_with(root_ == "/" ? "" : root_)) {
      std::string_view rest = std::string_view(h).substr(root_ == "/" ? 0 : root_.size());
      for (size_t i = 0; i < rest.size();) {
        size_t j = rest.find('/', i);
        if (j == std::string_view::npos) j = rest.size();
        if (j > i) home.emplace_back(rest.substr(i, j - i));
        i = j + 1;
      }
    }
    BigVec<uint32_t> order, pre(dirs), end(dirs);
    BigVec<int8_t> prior(dirs, 0);
    BigVec<uint8_t> depth(dirs, 0), home_at(dirs, 0);  // home components matched; 255 = diverged
    order.reserve(dirs);
    std::vector<std::pair<uint32_t, bool>> stack{{0, false}};
    while (!stack.empty()) {
      const auto [d, done] = stack.back();
      stack.pop_back();
      if (done) {
        end[d] = uint32_t(order.size());
        continue;
      }
      pre[d] = uint32_t(order.size());
      order.push_back(d);
      if (d != 0) {
        const uint32_t p = par[d];
        const std::string_view nm = names_.get(name_[raw[d]]);
        depth[d] = uint8_t(std::min(255, depth[p] + 1));
        const uint8_t h = home_at[p];
        home_at[d] = h == 255 ? 255 : h >= home.size() ? h : home[h] == nm ? uint8_t(h + 1) : 255;
        const bool entered = home_at[d] == home.size() && h < home.size();
        const int adj = prior_adjust(nm, depth[d], root_ == "/") + (entered ? 15 : 0);
        prior[d] = int8_t(std::clamp(prior[p] + adj, -100, 60));
      }
      stack.push_back({d, true});
      for (uint32_t i = start[d + 1]; i-- > start[d];) stack.push_back({kids[i], false});
    }
    b.fields[kDirPrior] = blob(std::move(prior));
    b.fields[kDirOrder] = blob(std::move(order));
    b.fields[kDirPre] = blob(std::move(pre));
    b.fields[kDirEnd] = blob(std::move(end));
  }

  // Splits names into directory and file sets, sorts each (text names, then
  // hex names), renumbers entries to match and lays out the index fields.
  Built finish() {
    names_.drop_table();
    const size_t n = name_.size(), names = names_.size();
    Built b;
    b.meta[kMetaN] = n;
    b.meta[kMetaBuildId] = new_build_id();

    BigVec<uint8_t> used(names, 0);  // bit 0: some directory, bit 1: some file
    uint32_t dirs = 0;
    for (size_t e = 0; e < n; ++e) {
      const bool dir = kind_[e] == kRawDir;
      used[name_[e]] |= dir ? 1 : 2;
      dirs += dir;
    }
    b.meta[kMetaDirs] = dirs;

    struct Set {
      BigVec<uint32_t> order;  // name ids: text names then hex names
      size_t text = 0;
      BigVec<uint32_t> rank;   // name id -> position in order
      BigVec<uint32_t> first;  // position -> first entry, plus end
    };
    std::array<Set, 2> sets;
    parallel(2, [&](size_t s) {
      Set& set = sets[s];
      BigVec<uint32_t> hex;
      for (uint32_t u = 0; u < names; ++u) {
        if (!(used[u] & (1 << s))) continue;
        (is_hex_name(names_.get(u)) ? hex : set.order).push_back(u);
      }
      sort_names(set.order);
      sort_names(hex);
      set.text = set.order.size();
      set.order.insert(set.order.end(), hex.begin(), hex.end());
      set.rank.assign(names, 0);
      for (uint32_t i = 0; i < set.order.size(); ++i) set.rank[set.order[i]] = i;
      set.first.assign(set.order.size() + 1, 0);
    });
    BigVec<uint8_t>().swap(used);

    for (size_t e = 0; e < n; ++e) {
      Set& set = sets[kind_[e] == kRawDir ? 0 : 1];
      ++set.first[set.rank[name_[e]] + 1];
    }
    sets[0].first[0] = 0;
    sets[1].first[0] = dirs;
    for (auto& set : sets)
      for (size_t i = 0; i + 1 < set.first.size(); ++i) set.first[i + 1] += set.first[i];

    parallel(4, [&](size_t j) {
      const size_t s = j / 2, hex = j % 2;
      const Set& set = sets[s];
      const size_t k0 = hex ? set.text : 0, k1 = hex ? set.order.size() : set.text;
      auto fields = make_section(
          k1 - k0, [&](size_t k) { return names_.get(set.order[k0 + k]); }, set.first.data() + k0, hex);
      for (size_t f = 0; f < kSectionFields; ++f)
        b.fields[kSections + (2 * s + hex) * kSectionFields + f] = std::move(fields[f]);
    });
    for (auto& set : sets) BigVec<uint32_t>().swap(set.order);

    BigVec<uint32_t> new_id(n);
    {
      std::array<BigVec<uint32_t>, 2> next;
      for (int s = 0; s < 2; ++s) next[size_t(s)].assign(sets[size_t(s)].first.begin(), sets[size_t(s)].first.end() - 1);
      for (size_t e = 0; e < n; ++e) {
        const int s = kind_[e] == kRawDir ? 0 : 1;
        new_id[e] = next[size_t(s)][sets[size_t(s)].rank[name_[e]]]++;
      }
    }
    for (auto& set : sets) BigVec<uint32_t>().swap(set.rank);
    lay_out_dirs(b, new_id, dirs);
    names_ = {};  // all name strings are now in the sections

    {
      BigVec<uint32_t> start(size_t(dirs) + 1, 0), kids(n - 1);
      BigVec<uint32_t> par(n);
      for (size_t e = 1; e < n; ++e) par[new_id[e]] = new_id[parent_[e]];
      for (uint32_t e = 1; e < n; ++e) ++start[par[e] + 1];
      for (uint32_t d = 0; d < dirs; ++d) start[d + 1] += start[d];
      BigVec<uint32_t> pos(start.begin(), start.end() - 1);
      for (uint32_t e = 1; e < n; ++e) kids[pos[par[e]]++] = e;  // ascending: names sorted, folders first
      b.fields[kChildStart] = blob(std::move(start));
      b.fields[kChildren] = blob(std::move(kids));
    }
    {
      BigVec<uint32_t> mtime(n);
      BigVec<uint8_t> flags(n);
      for (size_t e = 0; e < n; ++e) mtime[new_id[e]] = mtime_[e], flags[new_id[e]] = flags_[e];
      b.fields[kMtime] = blob(std::move(mtime));
      b.fields[kFlags] = blob(std::move(flags));
    }

    const uint32_t parent_bits = uint32_t(std::max(1, std::bit_width(std::max<uint32_t>(dirs, 1) - 1)));
    b.meta[kMetaParentBits] = parent_bits;
    {
      BigVec<uint8_t> parents((uint64_t(n) * parent_bits + 7) / 8 + 8, 0);
      BigVec<uint8_t> kind(n - dirs);
      BigVec<uint32_t> size(n - dirs);
      for (size_t e = 0; e < n; ++e) {
        const uint64_t bit = uint64_t(new_id[e]) * parent_bits;
        uint64_t v = uint64_t(new_id[parent_[e]]) << (bit & 7), w;
        std::memcpy(&w, parents.data() + (bit >> 3), sizeof w);
        w |= v;
        std::memcpy(parents.data() + (bit >> 3), &w, sizeof w);
        if (kind_[e] != kRawDir) {
          kind[new_id[e] - dirs] = kind_[e];
          size[new_id[e] - dirs] = size_[e];
        }
      }
      for (auto& o : big_) o.key = new_id[o.key];
      std::sort(big_.begin(), big_.end(), [](const Overflow& a, const Overflow& c) { return a.key < c.key; });
      b.fields[kParents] = blob(std::move(parents));
      b.fields[kFileKind] = blob(std::move(kind));
      b.fields[kFileSize] = blob(std::move(size));
      b.fields[kBigSizes] = blob(std::move(big_));
      for (CodeFile& c : code_) c.entry = new_id[c.entry];
      std::sort(code_.begin(), code_.end(), [](const CodeFile& a, const CodeFile& c) { return a.entry < c.entry; });
      BigVec<uint32_t> code_entry(code_.size());
      BigVec<uint64_t> code_mtime(code_.size());
      for (size_t k = 0; k < code_.size(); ++k) code_entry[k] = code_[k].entry, code_mtime[k] = code_[k].mtime;
      BigVec<CodeFile>().swap(code_);
      b.fields[kCodeEntry] = blob(std::move(code_entry));
      b.fields[kCodeMtime] = blob(std::move(code_mtime));
    }
    BigVec<uint32_t>().swap(new_id);
    name_.clear();
    parent_.clear();
    size_.clear();
    kind_.clear();
    mtime_.clear();
    flags_.clear();
    b.fields[kRoot] = blob(BigVec<char>(root_.begin(), root_.end()));
    return b;
  }

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

}  // namespace

bool has_full_disk_access() {
  // Readable only with Full Disk Access; without it, opening fails at once
  // rather than asking.
  static const bool fda = [] {
    const int fd = open("/Library/Application Support/com.apple.TCC/TCC.db", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) close(fd);
    return fd >= 0;
  }();
  return fda;
}

std::vector<std::string> excluded_dirs(const std::string& root) {
  std::vector<std::string> out;
  if (root == "/") {
    // Other volumes, the Data volume mirror behind the firmlinks, and devfs.
    out = {"/System/Volumes", "/Volumes", "/dev", "/net", "/home", "/private/var/vm"};
  }
  // Without Full Disk Access, opening these asks the user and blocks until
  // they answer, so they are left out instead.
  const char* home = std::getenv("HOME");
  if (!has_full_disk_access() && home)
    for (const char* d : {"Desktop", "Documents", "Downloads", "Library/Mobile Documents", "Library/Containers",
                          "Library/Group Containers", "Library/CloudStorage", "Pictures/Photos Library.photoslibrary"})
      out.push_back(std::string(home) + "/" + d);
  return out;
}

uint8_t categorize(std::string_view name) {
  size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return CatNone;
  size_t n = name.size() - dot - 1;
  if (n == 0 || n > 8) return CatNone;
  char buf[8];
  for (size_t i = 0; i < n; ++i) buf[i] = char(kFold[uint8_t(name[dot + 1 + i])]);
  auto& t = ext_table();
  auto it = t.find(std::string_view(buf, n));
  return it == t.end() ? CatNone : it->second;
}

size_t unpack_hex(const char* p, char* out, size_t& len) {
  len = uint8_t(p[0]);
  const uint8_t* src = reinterpret_cast<const uint8_t*>(p + 1);
  const size_t packed = (len + 1) / 2;
#if defined(__ARM_NEON)
  static const uint8_t digits[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                     '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
  const uint8x16_t table = vld1q_u8(digits);
  for (size_t i = 0; i < packed; i += 16) {
    uint8x16_t v = vld1q_u8(src + i);
    uint8x16_t hi = vqtbl1q_u8(table, vshrq_n_u8(v, 4));
    uint8x16_t lo = vqtbl1q_u8(table, vandq_u8(v, vdupq_n_u8(0x0f)));
    vst1q_u8(reinterpret_cast<uint8_t*>(out) + 2 * i, vzip1q_u8(hi, lo));
    vst1q_u8(reinterpret_cast<uint8_t*>(out) + 2 * i + 16, vzip2q_u8(hi, lo));
  }
#else
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < packed; ++i) {
    out[2 * i] = digits[src[i] >> 4];
    out[2 * i + 1] = digits[src[i] & 15];
  }
#endif
  return 1 + packed;
}

uint32_t Section::count(uint32_t name) const {
  const uint8_t c = counts[name];
  if (c != 255) return c;
  const Overflow* o = find_key(overflow, name);
  return o ? uint32_t(o->value) : c;
}

uint64_t Index::big_size(uint32_t e) const {
  const Overflow* o = find_key(big_sizes, e);
  return o ? o->value : UINT32_MAX;
}

std::string Index::name(uint32_t e) const {
  for (const Section& s : sections) {
    if (e < s.first_entry() || e >= s.end_entry()) continue;
    const uint32_t b = uint32_t(std::upper_bound(s.block_first.p, s.block_first.p + s.block_first.n, e) -
                                s.block_first.p - 1);
    uint32_t i = b * kBlock, acc = s.block_first[b];
    while (acc + s.count(i) <= e) acc += s.count(i++);
    const char* p = s.bytes.p + s.block_off[b] + s.name_off[i];
    if (!s.hex) return std::string(p);
    char buf[256 + 32];
    size_t len;
    unpack_hex(p, buf, len);
    return std::string(buf, len);
  }
  return {};
}

std::string_view Index::name_view(uint32_t e, char* buf) const {
  for (const Section& s : sections) {
    if (e < s.first_entry() || e >= s.end_entry()) continue;
    const uint32_t b = uint32_t(std::upper_bound(s.block_first.p, s.block_first.p + s.block_first.n, e) -
                                s.block_first.p - 1);
    uint32_t i = b * kBlock, acc = s.block_first[b];
    while (acc + s.count(i) <= e) acc += s.count(i++);
    const char* p = s.bytes.p + s.block_off[b] + s.name_off[i];
    if (!s.hex) return p;
    size_t len;
    unpack_hex(p, buf, len);
    return {buf, len};
  }
  return {};
}

uint32_t Index::child(uint32_t d, std::string_view name, bool any_case) const {
  // Children are ascending entry ids, and within each section entry ids
  // follow name order: binary search each section's run.
  const Span<uint32_t> k = kids(d);
  char buf[256 + 32];
  for (const Section& s : sections) {
    const uint32_t* lo = std::lower_bound(k.begin(), k.end(), s.first_entry());
    const uint32_t* hi = std::lower_bound(lo, k.end(), s.end_entry());
    const uint32_t* at = std::lower_bound(lo, hi, name, [&](uint32_t e, std::string_view n) { return name_view(e, buf) < n; });
    if (at != hi && name_view(*at, buf) == name) return *at;
  }
  if (any_case)
    for (uint32_t e : k) {
      const std::string_view nm = name_view(e, buf);
      if (nm.size() == name.size() &&
          std::equal(nm.begin(), nm.end(), name.begin(), [](char a, char b) { return kFold[uint8_t(a)] == kFold[uint8_t(b)]; }))
        return e;
    }
  return UINT32_MAX;
}

uint32_t Index::lookup(std::string_view p, bool any_case) const {
  const std::string_view r = root == "/" ? "" : std::string_view(root);
  if (!p.starts_with(r) || (p.size() > r.size() && p[r.size()] != '/')) return UINT32_MAX;
  p.remove_prefix(r.size());
  uint32_t e = 0;
  while (!p.empty()) {
    while (p.starts_with('/')) p.remove_prefix(1);
    if (p.empty()) break;
    const size_t slash = p.find('/');
    const std::string_view comp = p.substr(0, slash);
    p = slash == std::string_view::npos ? std::string_view() : p.substr(slash);
    if (!is_dir(e)) return UINT32_MAX;
    e = child(e, comp, any_case);
    if (e == UINT32_MAX) return e;
  }
  return e;
}

std::string Index::path(uint32_t e) const {
  if (e == 0) return root;
  uint32_t chain[1024];
  int depth = 0;
  for (uint32_t c = e; c != 0 && depth < 1024; c = parent(c)) chain[depth++] = c;
  std::string s = root == "/" ? "" : root;
  for (int j = depth - 1; j >= 0; --j) {
    s += '/';
    s += name(chain[j]);
  }
  return s;
}

namespace {

Index save_and_index_symbols(std::unique_ptr<Built> built, const std::string& cache_file, ScanProgress* progress,
                             bool background, const std::function<void(const Index&)>& on_names = {}) {
  Index ix;
  if (!save_fields(cache_file, kMagic, built->meta, built->fields) || !load_index(ix, cache_file))
    attach(ix, hold_fields(built->meta, std::move(built->fields)));  // could not write the cache
  built.reset();
  if (on_names) on_names(ix);
  build_symbols(ix, symbol_file(cache_file), progress, background);
  return ix;
}

}  // namespace

bool list_dir(const std::string& path, std::vector<Listed>& out) {
  out.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  thread_local std::vector<char> buf(256 * 1024);
  read_listing(fd, buf, [&](std::string_view name, uint32_t type, uint64_t size, uint64_t mtime, uint32_t bsd_flags,
                            uint32_t) {
    const bool dir = type == VDIR;
    out.push_back({std::string(name), dir, dir ? uint8_t(0) : uint8_t(size_class(size) << 4 | categorize(name)),
                   dir ? 0 : size, mtime, entry_flags(type, bsd_flags), type == VREG});
  });
  close(fd);
  return true;
}

Index build_index(const std::string& root, unsigned threads, ScanProgress* progress, bool background,
                  const std::string& cache_file, const std::function<void(const Index&)>& on_names) {
  std::string r = root;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  // Taken before the scan: replaying from here repeats changes the scan may
  // already have seen, which is harmless, and misses none.
  const uint64_t event_id = current_event_id();
  auto built = std::make_unique<Built>(Scanner(r, progress).run(threads ? threads : 1, background));
  built->meta[kMetaEventId] = event_id;
  return save_and_index_symbols(std::move(built), cache_file, progress, background, on_names);
}

Index refresh_index(const Index& old, const Changes& changes, unsigned threads, ScanProgress* progress,
                    bool background, const std::string& cache_file) {
  auto built =
      std::make_unique<Built>(Scanner(old.root, progress, &old, &changes).run(threads ? threads : 1, background));
  built->meta[kMetaEventId] = changes.event_id;
  return save_and_index_symbols(std::move(built), cache_file, progress, background);
}

bool load_index(Index& ix, const std::string& file) {
  Fields f;
  return map_fields(file, kMagic, kFields, f) && attach(ix, std::move(f));
}

}  // namespace fplussearch
