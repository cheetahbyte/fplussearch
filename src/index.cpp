#include "index.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/attr.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
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
    return m;
  }();
  return table;
}

bool is_hex_name(std::string_view s) {
  if (s.size() < 16 || s.size() > 255) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

void pack_hex(std::string_view s, std::vector<char>& out) {
  auto val = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  out.push_back(char(uint8_t(s.size())));
  for (size_t i = 0; i < s.size(); i += 2)
    out.push_back(char((val(s[i]) << 4) | (i + 1 < s.size() ? val(s[i + 1]) : 0)));
}

// 4 MB of anonymous memory. Mapped directly because malloc keeps freed
// medium-sized blocks resident, and the index build frees hundreds of MB.
constexpr size_t kMapChunk = size_t(4) << 20;
struct Unmap {
  void operator()(void* p) const { munmap(p, kMapChunk); }
};
using MapChunk = std::unique_ptr<void, Unmap>;

MapChunk map_chunk() {
  void* p = mmap(nullptr, kMapChunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  return MapChunk(p);
}

// Append-only array in fixed chunks: growth never copies, so building the
// index never holds two copies of an array.
template <typename T>
class Chunked {
 public:
  void push_back(const T& v) {
    if (n_ % kChunk == 0) chunks_.push_back(map_chunk());
    static_cast<T*>(chunks_.back().get())[n_ % kChunk] = v;
    ++n_;
  }
  const T& operator[](size_t i) const { return static_cast<const T*>(chunks_[i / kChunk].get())[i % kChunk]; }
  size_t size() const { return n_; }
  void clear() {
    std::vector<MapChunk>().swap(chunks_);
    n_ = 0;
  }

 private:
  static constexpr size_t kChunk = kMapChunk / sizeof(T);
  std::vector<MapChunk> chunks_;
  size_t n_ = 0;
};

// Deduplicates names as they are scanned. Not thread-safe.
class Interner {
 public:
  uint32_t intern(std::string_view s, uint32_t hash) {
    if (slots_.empty()) slots_.assign(size_t(1) << 20, 0);
    size_t mask = slots_.size() - 1;
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
      uint32_t slot = slots_[i];
      if (slot == 0) break;
      if (hash_[slot - 1] == hash && get(slot - 1) == s) return slot - 1;
    }
    const uint32_t id = uint32_t(ptr_.size());
    if (arena_.empty() || used_ + s.size() > kMapChunk) {
      arena_.push_back(map_chunk());
      used_ = 0;
    }
    char* dst = static_cast<char*>(arena_.back().get()) + used_;
    std::memcpy(dst, s.data(), s.size());
    used_ += s.size();
    ptr_.push_back(dst);
    len_.push_back(uint16_t(s.size()));
    hash_.push_back(hash);
    insert(id);
    if (ptr_.size() * 2 > slots_.size()) grow();
    return id;
  }

  std::string_view get(uint32_t id) const { return {ptr_[id], len_[id]}; }
  size_t size() const { return ptr_.size(); }
  void drop_table() { slots_ = {}; }

 private:
  void insert(uint32_t id) {
    size_t mask = slots_.size() - 1;
    size_t i = hash_[id] & mask;
    while (slots_[i]) i = (i + 1) & mask;
    slots_[i] = id + 1;
  }

  void grow() {
    slots_.assign(slots_.size() * 2, 0);
    for (uint32_t id = 0; id < ptr_.size(); ++id) insert(id);
  }

  std::vector<MapChunk> arena_;
  size_t used_ = 0;
  Chunked<const char*> ptr_;
  Chunked<uint16_t> len_;
  Chunked<uint32_t> hash_;
  std::vector<uint32_t> slots_;
};

// On-disk layout: a header with (offset, size) for each field, then the
// fields, each 64-byte aligned.
enum Field : int {
  kRoot = 0,
  kSections = 1,  // 6 fields per section, see section_field()
  kParents = kSections + 4 * 6,
  kFileKind,
  kFileSize,
  kBigSizes,
  kGrams,
  kFields
};
int section_field(int section, int f) { return kSections + section * 6 + f; }
enum SectionField : int { kBytes, kBlockOff, kBlockFirst, kNameOff, kCounts, kOverflow };

constexpr char kMagic[8] = {'F', 'S', 'R', 'C', 'H', 'I', 'X', '5'};
constexpr uint8_t kRawDir = 0xff;  // never a valid file kind byte

struct Header {
  char magic[8];
  uint32_t parent_bits;
  uint32_t unused;
  uint64_t n;
  uint64_t dirs;
  uint64_t field[kFields][2];
};

// A finished field: takes ownership of a vector without copying it.
struct Blob {
  std::shared_ptr<const void> owner;
  const char* data = nullptr;
  size_t size = 0;
};

struct Built {
  std::string root;
  uint32_t n = 0, dirs = 0, parent_bits = 0;
  std::array<Blob, kFields> fields;
};

template <typename T>
void put(Built& b, int field, std::vector<T>&& v) {
  auto owned = std::make_shared<const std::vector<T>>(std::move(v));
  b.fields[size_t(field)] = {owned, reinterpret_cast<const char*>(owned->data()), owned->size() * sizeof(T)};
}

template <typename T>
bool span_of(Span<T>& s, const char* base, size_t len, uint64_t off, uint64_t size) {
  if (off > len || size > len - off || off % alignof(T) || size % sizeof(T)) return false;
  s.p = reinterpret_cast<const T*>(base + off);
  s.n = size / sizeof(T);
  return true;
}

// Points `ix` at fields laid out as described by `fld` (offsets into base).
bool attach(Index& ix, const char* base, size_t len, uint32_t n, uint32_t dirs, uint32_t parent_bits,
            const uint64_t (*fld)[2]) {
  Span<char> root;
  bool ok = span_of(root, base, len, fld[kRoot][0], fld[kRoot][1]);
  if (!ok) return false;
  ix.root.assign(root.p, root.n);
  ix.n = n;
  ix.dirs = dirs;
  ix.parent_bits = parent_bits;
  for (int s = 0; s < 4; ++s) {
    Section& sec = ix.sections[size_t(s)];
    sec.hex = s % 2 == 1;
    auto f = [&](int k) { return fld[section_field(s, k)]; };
    ok = ok && span_of(sec.bytes, base, len, f(kBytes)[0], f(kBytes)[1]) &&
         span_of(sec.block_off, base, len, f(kBlockOff)[0], f(kBlockOff)[1]) &&
         span_of(sec.block_first, base, len, f(kBlockFirst)[0], f(kBlockFirst)[1]) &&
         span_of(sec.name_off, base, len, f(kNameOff)[0], f(kNameOff)[1]) &&
         span_of(sec.counts, base, len, f(kCounts)[0], f(kCounts)[1]) &&
         span_of(sec.overflow, base, len, f(kOverflow)[0], f(kOverflow)[1]) && sec.block_off.n > 0 &&
         sec.block_first.n == sec.block_off.n && sec.counts.n == sec.name_off.n;
  }
  ok = ok && span_of(ix.parents, base, len, fld[kParents][0], fld[kParents][1]) &&
       span_of(ix.file_kind, base, len, fld[kFileKind][0], fld[kFileKind][1]) &&
       span_of(ix.file_size, base, len, fld[kFileSize][0], fld[kFileSize][1]) &&
       span_of(ix.big_sizes, base, len, fld[kBigSizes][0], fld[kBigSizes][1]) &&
       span_of(ix.grams, base, len, fld[kGrams][0], fld[kGrams][1]) && ix.grams.n == 2 * kGramSlots &&
       ix.file_kind.n == n - dirs && ix.file_size.n == n - dirs && parent_bits >= 1 && parent_bits <= 32 &&
       ix.parents.n >= (uint64_t(n) * parent_bits + 7) / 8 + 8;
  ix.bytes = len;
  return ok;
}

// Fills in the header and returns the total file size.
uint64_t layout(const Built& b, Header& h) {
  h = {};
  std::memcpy(h.magic, kMagic, sizeof kMagic);
  h.parent_bits = b.parent_bits;
  h.n = b.n;
  h.dirs = b.dirs;
  uint64_t off = (sizeof h + 63) & ~uint64_t(63);
  for (int i = 0; i < kFields; ++i) {
    h.field[i][0] = off;
    h.field[i][1] = b.fields[size_t(i)].size;
    off = (off + h.field[i][1] + 63) & ~uint64_t(63);
  }
  return off;
}

bool save(const Built& b, const std::string& file) {
  Header h;
  layout(b, h);
  std::string tmp = file + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) return false;
  static const char zeros[64] = {};
  bool ok = std::fwrite(&h, sizeof h, 1, f) == 1;
  uint64_t pos = sizeof h;
  for (int i = 0; i < kFields && ok; ++i) {
    ok = std::fwrite(zeros, 1, h.field[i][0] - pos, f) == h.field[i][0] - pos;
    const Blob& v = b.fields[size_t(i)];
    ok = ok && std::fwrite(v.data, 1, v.size, f) == v.size;
    pos = h.field[i][0] + v.size;
  }
  ok &= std::fclose(f) == 0;
  if (!ok || std::rename(tmp.c_str(), file.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

struct Work {
  uint32_t id;
  std::string path;
};

struct Item {
  uint32_t off;  // into the batch's name buffer
  uint16_t len;
  uint32_t hash;
  uint64_t size;
  uint8_t kind;
};

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
  Scanner(std::string root, ScanProgress* progress) : root_(std::move(root)), progress_(progress) {
    if (root_ == "/") {
      // Other volumes, the Data volume mirror behind the firmlinks, and devfs.
      excluded_ = {"/System/Volumes", "/Volumes", "/dev", "/net", "/home", "/private/var/vm"};
    }
  }

  Built run(unsigned threads, bool background) {
    add_entry(names_.intern("", 0), 0, 0, kRawDir);
    stack_.push_back({0, root_});
    pending_ = 1;

    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; ++i)
      pool.emplace_back([this, background] {
        if (background) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        worker();
      });
    for (auto& t : pool) t.join();
    return finish();
  }

 private:
  void add_entry(uint32_t name, uint32_t parent, uint64_t size, uint8_t kind) {
    if (kind != kRawDir && size >= UINT32_MAX) big_.push_back({uint32_t(name_.size()), 0, size});
    name_.push_back(name);
    parent_.push_back(parent);
    size_.push_back(uint32_t(std::min<uint64_t>(size, UINT32_MAX)));
    kind_.push_back(kind);
  }

  void worker() {
    std::vector<char> buf(256 * 1024);
    std::vector<char> names;
    std::vector<Item> items;
    std::vector<std::pair<uint32_t, std::string>> subdirs;
    for (;;) {
      Work w;
      {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return !stack_.empty() || pending_ == 0; });
        if (stack_.empty()) return;
        w = std::move(stack_.back());
        stack_.pop_back();
      }
      read_dir(w, buf, names, items, subdirs);
      publish(w, names, items, subdirs);
    }
  }

  void read_dir(const Work& w, std::vector<char>& buf, std::vector<char>& names, std::vector<Item>& items,
                std::vector<std::pair<uint32_t, std::string>>& subdirs) {
    names.clear();
    items.clear();
    subdirs.clear();
    int fd = open(w.path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;

    attrlist al{};
    al.bitmapcount = ATTR_BIT_MAP_COUNT;
    al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE;
    al.fileattr = ATTR_FILE_DATALENGTH;

    for (;;) {
      int n = getattrlistbulk(fd, &al, buf.data(), buf.size(), 0);
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
        size_t nl = strnlen(nm, ref.attr_length);
        f += sizeof ref;
        uint32_t type = VNON;
        if (ret.commonattr & ATTR_CMN_OBJTYPE) {
          std::memcpy(&type, f, sizeof type);
          f += sizeof type;
        }
        uint64_t size = 0;
        if (ret.fileattr & ATTR_FILE_DATALENGTH) {
          off_t v;
          std::memcpy(&v, f, sizeof v);
          size = uint64_t(v);
        }
        if (nl == 0 || nl > 1023) continue;

        std::string_view name(nm, nl);
        const bool dir = type == VDIR;
        const uint8_t kind = dir ? kRawDir : uint8_t(size_class(size) << 4 | categorize(name));
        items.push_back({uint32_t(names.size()), uint16_t(nl), uint32_t(std::hash<std::string_view>{}(name)),
                         size, kind});
        names.insert(names.end(), nm, nm + nl);
        if (dir) {
          std::string child = join(w.path, name);
          bool skip = false;
          for (const auto& ex : excluded_) skip |= child == ex;
          if (!skip) subdirs.emplace_back(uint32_t(items.size() - 1), std::move(child));
        }
      }
    }
    close(fd);
  }

  void publish(const Work& w, const std::vector<char>& names, const std::vector<Item>& items,
               std::vector<std::pair<uint32_t, std::string>>& subdirs) {
    std::lock_guard lk(m_);
    const uint32_t base = uint32_t(name_.size());
    if (base + items.size() < UINT32_MAX) {
      for (const auto& it : items)
        add_entry(names_.intern({names.data() + it.off, it.len}, it.hash), w.id, it.size, it.kind);
      for (auto& [local, path] : subdirs) stack_.push_back({base + local, std::move(path)});
      pending_ += subdirs.size();
      if (progress_) progress_->entries.store(name_.size(), std::memory_order_relaxed);
    }
    if (--pending_ == 0 || !subdirs.empty()) cv_.notify_all();
  }

  // Splits names into directory and file sets, sorts each (text names, then
  // hex names), renumbers entries to match and lays out the index fields.
  Built finish() {
    names_.drop_table();
    const size_t n = name_.size(), names = names_.size();
    Built b;
    b.root = root_;
    b.n = uint32_t(n);

    std::vector<uint8_t> used(names, 0);  // bit 0: some directory, bit 1: some file
    uint32_t dirs = 0;
    for (size_t e = 0; e < n; ++e) {
      const bool dir = kind_[e] == kRawDir;
      used[name_[e]] |= dir ? 1 : 2;
      dirs += dir;
    }
    b.dirs = dirs;

    struct Set {
      std::vector<uint32_t> order;  // name ids: text names then hex names
      size_t text = 0;
      std::vector<uint32_t> rank;   // name id -> position in order
      std::vector<uint32_t> first;  // position -> first entry, plus end
    };
    std::array<Set, 2> sets;
    for (int s = 0; s < 2; ++s) {
      Set& set = sets[size_t(s)];
      std::vector<uint32_t> hex;
      for (uint32_t u = 0; u < names; ++u) {
        if (!(used[u] & (1 << s))) continue;
        (is_hex_name(names_.get(u)) ? hex : set.order).push_back(u);
      }
      auto by_name = [&](uint32_t a, uint32_t c) { return names_.get(a) < names_.get(c); };
      std::sort(set.order.begin(), set.order.end(), by_name);
      std::sort(hex.begin(), hex.end(), by_name);
      set.text = set.order.size();
      set.order.insert(set.order.end(), hex.begin(), hex.end());
      set.rank.assign(names, 0);
      for (uint32_t i = 0; i < set.order.size(); ++i) set.rank[set.order[i]] = i;
      set.first.assign(set.order.size() + 1, 0);
    }
    used = {};

    for (size_t e = 0; e < n; ++e) {
      Set& set = sets[kind_[e] == kRawDir ? 0 : 1];
      ++set.first[set.rank[name_[e]] + 1];
    }
    sets[0].first[0] = 0;
    sets[1].first[0] = dirs;
    for (auto& set : sets)
      for (size_t i = 0; i + 1 < set.first.size(); ++i) set.first[i + 1] += set.first[i];

    {
      std::vector<uint32_t> grams(2 * kGramSlots, 0);
      std::vector<uint32_t> seen(kGramSlots, 0);
      uint32_t stamp = 0;
      for (int s = 0; s < 2; ++s) {
        const Set& set = sets[size_t(s)];
        uint32_t* g = grams.data() + size_t(s) * kGramSlots;
        for (size_t k = 0; k < set.order.size(); ++k) {
          const uint32_t c = set.first[k + 1] - set.first[k];
          std::string_view nm = names_.get(set.order[k]);
          ++stamp;
          for (size_t i = 0; i < nm.size(); ++i) {
            const uint32_t a = kFold[uint8_t(nm[i])];
            if (seen[a] != stamp) seen[a] = stamp, g[a] += c;
            if (i + 1 < nm.size()) {
              const uint32_t bg = 256 + (a << 8 | kFold[uint8_t(nm[i + 1])]);
              if (seen[bg] != stamp) seen[bg] = stamp, g[bg] += c;
            }
          }
        }
      }
      put(b, kGrams, std::move(grams));
    }

    for (int s = 0; s < 2; ++s) {
      const Set& set = sets[size_t(s)];
      write_section(b, 2 * s, set, 0, set.text, false);
      write_section(b, 2 * s + 1, set, set.text, set.order.size(), true);
    }
    names_ = {};  // all name strings are now in the sections
    for (auto& set : sets) set.order = {};

    std::vector<uint32_t> new_id(n);
    {
      std::array<std::vector<uint32_t>, 2> next;
      for (int s = 0; s < 2; ++s) next[size_t(s)].assign(sets[size_t(s)].first.begin(), sets[size_t(s)].first.end() - 1);
      for (size_t e = 0; e < n; ++e) {
        const int s = kind_[e] == kRawDir ? 0 : 1;
        new_id[e] = next[size_t(s)][sets[size_t(s)].rank[name_[e]]]++;
      }
    }
    for (auto& set : sets) set.rank = {};

    b.parent_bits = uint32_t(std::max(1, std::bit_width(std::max<uint32_t>(dirs, 1) - 1)));
    {
      std::vector<uint8_t> parents((uint64_t(n) * b.parent_bits + 7) / 8 + 8, 0);
      std::vector<uint8_t> kind(n - dirs);
      std::vector<uint32_t> size(n - dirs);
      for (size_t e = 0; e < n; ++e) {
        const uint64_t bit = uint64_t(new_id[e]) * b.parent_bits;
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
      put(b, kParents, std::move(parents));
      put(b, kFileKind, std::move(kind));
      put(b, kFileSize, std::move(size));
      put(b, kBigSizes, std::move(big_));
    }
    new_id = {};
    name_.clear();
    parent_.clear();
    size_.clear();
    kind_.clear();
    put(b, kRoot, std::vector<char>(root_.begin(), root_.end()));
    return b;
  }

  template <typename SetT>
  void write_section(Built& b, int section, const SetT& set, size_t k0, size_t k1, bool hex) {
    std::vector<char> bytes;
    std::vector<uint32_t> block_off, block_first;
    std::vector<uint16_t> name_off;
    std::vector<uint8_t> counts;
    std::vector<Overflow> overflow;
    size_t total = kPad;
    for (size_t k = k0; k < k1; ++k) {
      const size_t len = names_.get(set.order[k]).size();
      total += hex ? 1 + (len + 1) / 2 : len + 1;
    }
    bytes.reserve(total);
    block_off.reserve((k1 - k0) / kBlock + 2);
    block_first.reserve((k1 - k0) / kBlock + 2);
    name_off.reserve(k1 - k0);
    counts.reserve(k1 - k0);
    size_t block_start = 0;
    for (size_t k = k0; k < k1; ++k) {
      const uint32_t i = uint32_t(k - k0);
      if (i % kBlock == 0) {
        block_start = bytes.size();
        block_off.push_back(uint32_t(block_start));
        block_first.push_back(set.first[k]);
      }
      name_off.push_back(uint16_t(bytes.size() - block_start));
      const uint32_t c = set.first[k + 1] - set.first[k];
      counts.push_back(uint8_t(std::min<uint32_t>(c, 255)));
      if (c >= 255) overflow.push_back({i, 0, c});
      std::string_view nm = names_.get(set.order[k]);
      if (hex) {
        pack_hex(nm, bytes);
      } else {
        bytes.insert(bytes.end(), nm.begin(), nm.end());
        bytes.push_back('\0');
      }
    }
    block_off.push_back(uint32_t(bytes.size()));
    block_first.push_back(set.first[k1]);
    bytes.resize(bytes.size() + kPad, '\0');
    put(b, section_field(section, kBytes), std::move(bytes));
    put(b, section_field(section, kBlockOff), std::move(block_off));
    put(b, section_field(section, kBlockFirst), std::move(block_first));
    put(b, section_field(section, kNameOff), std::move(name_off));
    put(b, section_field(section, kCounts), std::move(counts));
    put(b, section_field(section, kOverflow), std::move(overflow));
  }

  std::string root_;
  ScanProgress* progress_;
  std::vector<std::string> excluded_;
  std::mutex m_;
  std::condition_variable cv_;
  std::vector<Work> stack_;
  size_t pending_ = 0;

  Interner names_;
  Chunked<uint32_t> name_, parent_, size_;
  Chunked<uint8_t> kind_;
  std::vector<Overflow> big_;
};

const Overflow* find_key(const Span<Overflow>& s, uint32_t key) {
  const Overflow* it = std::lower_bound(s.p, s.p + s.n, key, [](const Overflow& o, uint32_t k) { return o.key < k; });
  return it != s.p + s.n && it->key == key ? it : nullptr;
}

}  // namespace

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

Index build_index(const std::string& root, unsigned threads, ScanProgress* progress, bool background,
                  const std::string& cache_file) {
  std::string r = root;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  auto built = std::make_unique<Built>(Scanner(r, progress).run(threads ? threads : 1, background));

  Index ix;
  if (save(*built, cache_file) && load_index(ix, cache_file)) return ix;

  // Could not write the cache: serve the same layout from memory.
  Header h;
  auto buf = std::make_shared<std::vector<char>>(layout(*built, h), '\0');
  std::memcpy(buf->data(), &h, sizeof h);
  for (int i = 0; i < kFields; ++i)
    if (h.field[i][1]) std::memcpy(buf->data() + h.field[i][0], built->fields[size_t(i)].data, h.field[i][1]);
  built.reset();
  attach(ix, buf->data(), buf->size(), uint32_t(h.n), uint32_t(h.dirs), h.parent_bits, h.field);
  ix.backing = std::move(buf);
  return ix;
}

bool load_index(Index& ix, const std::string& file) {
  int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st{};
  if (fstat(fd, &st) != 0 || size_t(st.st_size) < sizeof(Header)) {
    close(fd);
    return false;
  }
  const size_t len = size_t(st.st_size);
  void* map = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) return false;
  madvise(map, len, MADV_WILLNEED);
  std::shared_ptr<const void> backing(map, [len](const void* p) { munmap(const_cast<void*>(p), len); });

  const char* base = static_cast<const char*>(map);
  Header h;
  std::memcpy(&h, base, sizeof h);
  if (std::memcmp(h.magic, kMagic, sizeof kMagic) != 0 || h.n > UINT32_MAX || h.dirs > h.n) return false;
  Index tmp;
  if (!attach(tmp, base, len, uint32_t(h.n), uint32_t(h.dirs), h.parent_bits, h.field)) return false;
  tmp.backing = std::move(backing);
  ix = std::move(tmp);
  return true;
}

}  // namespace fplussearch
