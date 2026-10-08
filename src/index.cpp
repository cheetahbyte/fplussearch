#include "index.hpp"

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

// Field order in the cache file.
enum Field : size_t {
  kRoot = 0,
  kSections = 1,  // kSectionFields per section
  kParents = kSections + 4 * kSectionFields,
  kFileKind,
  kFileSize,
  kBigSizes,
  kGrams,
  kFields
};
enum Meta : size_t { kMetaN, kMetaDirs, kMetaParentBits, kMetaBuildId };

constexpr char kMagic[8] = {'F', 'S', 'R', 'C', 'H', 'I', 'X', '6'};
constexpr uint8_t kRawDir = 0xff;  // never a valid file kind byte

struct Built {
  std::array<uint64_t, 8> meta{};
  std::vector<Blob> fields = std::vector<Blob>(kFields);
  BigVec<CodeFile> code;  // source files for the symbol index, by entry
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
  bool ok = f.meta[kMetaN] <= UINT32_MAX && r.dirs <= r.n;
  for (size_t s = 0; s < 4; ++s) ok = ok && f.section(kSections + s * kSectionFields, r.sections[s], s % 2 == 1);
  ok = ok && f.get(kParents, r.parents) && f.get(kFileKind, r.file_kind) && f.get(kFileSize, r.file_size) &&
       f.get(kBigSizes, r.big_sizes) && f.get(kGrams, r.grams) && r.grams.n == 2 * kGramSlots &&
       r.file_kind.n == r.n - r.dirs && r.file_size.n == r.n - r.dirs && r.parent_bits >= 1 &&
       r.parent_bits <= 32 && r.parents.n >= (uint64_t(r.n) * r.parent_bits + 7) / 8 + 8;
  if (!ok) return false;
  r.bytes = f.bytes;
  r.backing = std::move(f.backing);
  ix = std::move(r);
  return true;
}

uint64_t new_build_id() {
  return hash_bytes(uint64_t(std::chrono::system_clock::now().time_since_epoch().count()),
                    std::to_string(std::random_device{}()));
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
  uint64_t mtime;  // nanoseconds; only kept for source files
  uint8_t kind;
  bool code;
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
    al.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME;
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
        uint64_t mtime = 0;
        if (ret.commonattr & ATTR_CMN_MODTIME) {
          timespec ts;
          std::memcpy(&ts, f, sizeof ts);
          f += sizeof ts;
          mtime = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
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
        const bool code = !dir && type == VREG && size <= kMaxSourceSize && lang_of(name) != Lang::None;
        items.push_back({uint32_t(names.size()), uint16_t(nl), uint32_t(std::hash<std::string_view>{}(name)),
                         size, mtime, kind, code});
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
      for (const auto& it : items) {
        if (it.code) code_.push_back({uint32_t(name_.size()), it.mtime});
        add_entry(names_.intern({names.data() + it.off, it.len}, it.hash), w.id, it.size, it.kind);
      }
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
    for (int s = 0; s < 2; ++s) {
      Set& set = sets[size_t(s)];
      BigVec<uint32_t> hex;
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
      BigVec<uint32_t> grams(2 * kGramSlots, 0);
      BigVec<uint32_t> seen(kGramSlots, 0);
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
      b.fields[kGrams] = blob(std::move(grams));
    }

    for (size_t s = 0; s < 2; ++s) {
      const Set& set = sets[s];
      for (size_t hex = 0; hex < 2; ++hex) {
        const size_t k0 = hex ? set.text : 0, k1 = hex ? set.order.size() : set.text;
        auto fields = make_section(
            k1 - k0, [&](size_t k) { return names_.get(set.order[k0 + k]); }, set.first.data() + k0, hex);
        for (size_t f = 0; f < kSectionFields; ++f)
          b.fields[kSections + (2 * s + hex) * kSectionFields + f] = std::move(fields[f]);
      }
    }
    names_ = {};  // all name strings are now in the sections
    for (auto& set : sets) set.order = {};

    BigVec<uint32_t> new_id(n);
    {
      std::array<BigVec<uint32_t>, 2> next;
      for (int s = 0; s < 2; ++s) next[size_t(s)].assign(sets[size_t(s)].first.begin(), sets[size_t(s)].first.end() - 1);
      for (size_t e = 0; e < n; ++e) {
        const int s = kind_[e] == kRawDir ? 0 : 1;
        new_id[e] = next[size_t(s)][sets[size_t(s)].rank[name_[e]]]++;
      }
    }
    for (auto& set : sets) set.rank = {};

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
      b.code.reserve(code_.size());
      for (const CodeFile& c : code_) b.code.push_back({new_id[c.entry], c.mtime});
      BigVec<CodeFile>().swap(code_);
      std::sort(b.code.begin(), b.code.end(), [](const CodeFile& a, const CodeFile& c) { return a.entry < c.entry; });
    }
    new_id = {};
    name_.clear();
    parent_.clear();
    size_.clear();
    kind_.clear();
    b.fields[kRoot] = blob(BigVec<char>(root_.begin(), root_.end()));
    return b;
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
  BigVec<Overflow> big_;
  BigVec<CodeFile> code_;  // raw entry ids until finish()
};

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
  BigVec<CodeFile> code = std::move(built->code);

  Index ix;
  if (!save_fields(cache_file, kMagic, built->meta, built->fields) || !load_index(ix, cache_file))
    attach(ix, hold_fields(built->meta, std::move(built->fields)));  // could not write the cache
  built.reset();
  build_symbols(ix, code.data(), code.size(), symbol_file(cache_file), progress, background);
  return ix;
}

bool load_index(Index& ix, const std::string& file) {
  Fields f;
  return map_fields(file, kMagic, kFields, f) && attach(ix, std::move(f));
}

}  // namespace fplussearch
