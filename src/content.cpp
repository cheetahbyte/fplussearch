#include "content.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <queue>
#include <thread>
#include <deque>
#include <unordered_set>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "pool.hpp"
#include "regex.hpp"
#include "store.hpp"

namespace fplussearch {

struct DocIn {
  std::string_view path;  // into a PathArena
  uint64_t size;
  uint32_t mtime;
};

// Paths stored back to back: a million of them as separate strings would
// cost twice the memory, most of it allocator overhead. The buffer is
// reserved, never grown, so views into it stay valid (unwritten reserved
// pages cost nothing).
class PathArena {
 public:
  PathArena() { bytes_.reserve(kCapacity); }
  std::string_view add(std::string_view path) {
    if (bytes_.size() + path.size() > kCapacity) return overflow_.emplace_back(path);
    const size_t at = bytes_.size();
    bytes_.insert(bytes_.end(), path.begin(), path.end());
    return {bytes_.data() + at, path.size()};
  }

  void clear() {
    BigVec<char>().swap(bytes_);
    std::deque<std::string>().swap(overflow_);
  }

 private:
  static constexpr size_t kCapacity = size_t(1) << 30;
  BigVec<char> bytes_;
  std::deque<std::string> overflow_;  // stable addresses past the reserve
};

namespace {

constexpr char kMagic[8] = {'F', 'P', 'S', 'C', 'O', 'N', 'T', '2'};
enum Field : size_t { kKey, kOff, kPost, kPathOff, kPaths, kSize, kMtime, kByPath, kRank, kFields };
constexpr uint32_t kBitset = 0x80000000u;  // tri_off flag: the list is a bitset over docs
constexpr int8_t kNotText = INT8_MIN;      // looked at, not text: never a candidate
constexpr uint64_t kBatchBytes = uint64_t(32) << 20;  // file bytes per segment build
constexpr size_t kMergeTier = 8;             // segments of a size tier merged into one
constexpr uint64_t kMergeCap = uint64_t(256) << 20;  // largest merge, in posting bytes
constexpr size_t kMaxSegments = 6;

uint8_t fold(uint8_t b) { return kFold[b]; }

// ---------------------------------------------------------------- eligibility

// Folders whose contents are generated, package stores or caches: never
// the text someone searches for, and often huge. Unlike build/, vendor/,
// dist/ and nested Library/ folders, which hold real source in many trees.
constexpr std::string_view kSkipDirs[] = {
    "node_modules", ".git", "target", "DerivedData", "__pycache__", ".venv", "venv", "site-packages", "Pods", ".next",
    ".turbo", ".cache", ".build", ".rustup", ".cargo", ".npm", ".bun", ".nvm", ".pnpm-store", "coverage", ".Trash",
    ".svn", ".hg", ".gradle", ".m2", ".pyenv", ".rbenv", ".gem", ".conda", "miniconda3", "anaconda3", ".docker",
    ".orbstack", ".colima", ".lima", ".ollama", ".android", ".expo", ".terraform.d", ".wrangler", ".vscode-server",
    "cache", "Cache", "caches", "Caches"};
// Folders right under home (paths relative to it): app data and toolchains.
constexpr std::string_view kSkipUnderHome[] = {
    "Library", "go/pkg", ".cursor/extensions", ".vscode/extensions", ".local/share", ".local/state", ".config/gcloud",
    ".codex/.tmp"};
constexpr std::string_view kSkipSuffixes[] = {
    ".app", ".photoslibrary", ".library", ".lrlibrary", ".musiclibrary", ".tvlibrary", ".imovielibrary", ".xcassets",
    ".framework", ".bundle", ".xcarchive", ".xcresult", ".dSYM", ".salon", ".lrdata"};
// Extensions that are never text: not worth an open.
constexpr std::string_view kBinaryExts[] = {
    "png", "jpg", "jpeg", "gif", "heic", "heif", "webp", "tif", "tiff", "bmp", "ico", "icns", "psd", "avif", "jxl",
    "raw", "cr2", "cr3", "nef", "arw", "dng", "mp4", "mov", "m4v", "mkv", "avi", "webm", "wmv", "flv", "mpg", "mpeg",
    "mp3", "m4a", "aac", "wav", "flac", "aiff", "aif", "ogg", "opus", "caf", "mid", "midi", "zip", "tar", "gz", "tgz",
    "bz2", "xz", "7z", "rar", "dmg", "iso", "zst", "lz4", "xip", "pkg", "jar", "war", "whl", "egg", "apk", "ipa",
    "ttf", "otf", "woff", "woff2", "ttc", "eot", "o", "a", "so", "dylib", "dll", "exe", "obj", "lib", "class", "pyc",
    "pyo", "wasm", "node", "bin", "dat", "db", "sqlite", "sqlite3", "db-wal", "db-shm", "pdf", "doc", "docx", "xls",
    "xlsx", "ppt", "pptx", "pages", "numbers", "key", "odt", "ods", "epub", "pak", "car", "nib", "mlmodel",
    "onnx", "pt", "pth", "ckpt", "safetensors", "gguf", "npy", "npz", "parquet", "arrow", "feather", "h5", "hdf5",
    "tfrecord", "pb", "idx", "pack", "keystore", "p12", "der", "cer", "mobileprovision", "dex", "res", "icc",
    "ktx", "dds", "exr", "hdr", "blend", "fbx", "glb", "usdz", "unitypackage", "ai", "sketch", "fig", "xcf"};

bool eq_fold(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (fold(uint8_t(a[i])) != fold(uint8_t(b[i]))) return false;
  return true;
}

// `rel`: the folder's path below home; `name`: its last component.
bool skip_dir(std::string_view rel, std::string_view name) {
  for (auto x : kSkipDirs)
    if (name == x) return true;
  for (auto x : kSkipUnderHome)
    if (rel == x) return true;
  for (auto x : kSkipSuffixes)
    if (name.size() > x.size() && name.ends_with(x)) return true;
  return false;
}

// The name half of eligibility: a file worth opening to see if it's text.
bool file_ok(std::string_view name, uint64_t size) {
  if (size > kMaxContentFile || size == 0) return false;
  if (name.ends_with(".min.js") || name == "package-lock.json" || name == ".DS_Store") return false;
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return true;
  const std::string_view ext = name.substr(dot + 1);
  if (ext.size() > 16) return true;
  for (auto x : kBinaryExts)
    if (eq_fold(ext, x)) return false;
  return true;
}

// Candidate order tier: your files first, then dot-folders, logs and locks.
int8_t doc_rank(std::string_view path) {
  int8_t r = 0;
  if (path.find("/.") != std::string_view::npos) r -= 2;
  const std::string_view name = path.substr(path.rfind('/') + 1);
  for (std::string_view x : {".jsonl", ".ndjson", ".log", ".lock", ".sum"})
    if (name.ends_with(x)) return int8_t(r - 1);
  return r;
}

// Whether `path` is inside `home` and no folder on the way is skipped.
bool path_in_scope(std::string_view path, std::string_view home) {
  if (!path.starts_with(home) || (path.size() > home.size() && path[home.size()] != '/')) return false;
  if (path.size() <= home.size() + 1) return true;
  const std::string_view rel = path.substr(home.size() + 1);
  for (size_t at = 0;;) {
    const size_t slash = rel.find('/', at);
    if (slash == std::string_view::npos) return true;  // the last component is the file itself
    if (skip_dir(rel.substr(0, slash), rel.substr(at, slash - at))) return false;
    at = slash + 1;
  }
}

// ---------------------------------------------------------------- files

// Opens a regular file without blocking (O_NONBLOCK: a FIFO never hangs
// open) and reads up to `cap` bytes into `buf`.
bool read_file(const char* path, BigVec<char>& buf, size_t cap, struct stat* out = nullptr) {
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  if (out) *out = st;
  const size_t want = std::min<uint64_t>(uint64_t(st.st_size), cap);
  buf.resize(want);
  size_t got = 0;
  while (got < want) {
    const ssize_t n = read(fd, buf.data() + got, want - got);
    if (n <= 0) break;
    got += size_t(n);
  }
  close(fd);
  buf.resize(got);
  return true;
}

bool looks_binary(const BigVec<char>& b) {
  return std::memchr(b.data(), 0, std::min<size_t>(b.size(), 8192)) != nullptr;
}

void put_varint(std::vector<uint8_t>& out, uint32_t v) {
  while (v >= 0x80) {
    out.push_back(uint8_t(v | 0x80));
    v >>= 7;
  }
  out.push_back(uint8_t(v));
}

const uint8_t* get_varint(const uint8_t* p, uint32_t& v) {
  v = 0;
  for (int s = 0;; s += 7) {
    const uint8_t b = *p++;
    v |= uint32_t(b & 0x7f) << s;
    if (!(b & 0x80) || s >= 28) return p;
  }
}

// Each posting carries which of 7 buckets the character after the trigram
// falls in, anywhere in the doc: nearly 4-gram precision for a byte.
uint8_t follow_bit(uint8_t folded) { return uint8_t(1u << (folded % 7)); }

// The doc's distinct case-folded trigrams as (trigram << 8 | follow mask),
// sorted, appended to `out`. `masks` is a 16 MB table over the trigram
// space, all zero on entry and exit.
// Appends the distinct trigrams of buf to `out`, sorted, each as
// trigram << 8 | the follow bits of every character seen after it. Sorting
// a scratch list costs a little CPU but no per-thread 16 MB table.
void trigrams(const char* buf, size_t n, std::vector<uint32_t>& scratch, BigVec<uint32_t>& out) {
  if (n < 3) return;
  scratch.clear();
  uint32_t t = uint32_t(fold(uint8_t(buf[0]))) << 8 | fold(uint8_t(buf[1]));
  for (size_t i = 2; i < n; ++i) {
    t = ((t << 8) | fold(uint8_t(buf[i]))) & 0xffffff;
    scratch.push_back(t << 8 | (i + 1 < n ? follow_bit(fold(uint8_t(buf[i + 1]))) : 0));
  }
  std::sort(scratch.begin(), scratch.end());
  for (size_t i = 0; i < scratch.size();) {
    uint32_t v = scratch[i];
    for (++i; i < scratch.size() && (scratch[i] >> 8) == (v >> 8); ++i) v |= scratch[i] & 0xff;
    out.push_back(v);
  }
}

// A pattern's trigrams with the follow bits every doc must have.
std::vector<std::pair<uint32_t, uint8_t>> trigrams_small(std::string_view s) {
  std::vector<std::pair<uint32_t, uint8_t>> t;
  for (size_t i = 0; i + 3 <= s.size(); ++i) {
    const uint32_t tri =
        uint32_t(fold(uint8_t(s[i]))) << 16 | uint32_t(fold(uint8_t(s[i + 1]))) << 8 | fold(uint8_t(s[i + 2]));
    t.push_back({tri, i + 3 < s.size() ? follow_bit(fold(uint8_t(s[i + 3]))) : uint8_t(0)});
  }
  std::sort(t.begin(), t.end());
  std::vector<std::pair<uint32_t, uint8_t>> out;
  for (auto& x : t) {
    if (!out.empty() && out.back().first == x.first) out.back().second |= x.second;
    else out.push_back(x);
  }
  return out;
}

// ---------------------------------------------------------------- plans

// Which documents could possibly match: a tree of trigram tests.
struct TQ {
  enum Kind : uint8_t { All, Tri, And, Or } kind = All;
  uint32_t tri = 0;
  uint8_t need = 0;  // follow bits a doc must have for `tri`
  std::vector<TQ> kids;
};

TQ tq_and(TQ a, TQ b) {
  if (a.kind == TQ::All) return b;
  if (b.kind == TQ::All) return a;
  TQ r;
  r.kind = TQ::And;
  for (TQ* x : {&a, &b}) {
    if (x->kind == TQ::And) r.kids.insert(r.kids.end(), x->kids.begin(), x->kids.end());
    else r.kids.push_back(std::move(*x));
  }
  return r;
}

TQ literal_plan(std::string_view s) {
  TQ q;
  if (s.size() < 3) return q;
  q.kind = TQ::And;
  for (auto [t, need] : trigrams_small(s)) q.kids.push_back({TQ::Tri, t, need, {}});
  return q;
}

// What a regex fragment says about matching text: a small set of exact
// (folded) strings it can match, or failing that, a trigram query.
struct Info {
  std::optional<std::vector<std::string>> exact;
  TQ q;
};
constexpr size_t kMaxExact = 16;

TQ exact_query(const std::optional<std::vector<std::string>>& set) {
  TQ q;
  if (!set || set->empty()) return q;
  for (const auto& s : *set)
    if (s.size() < 3) return q;
  if (set->size() == 1) return literal_plan((*set)[0]);
  q.kind = TQ::Or;
  for (const auto& s : *set) q.kids.push_back(literal_plan(s));
  return q;
}

Info info_all() { return {std::nullopt, TQ{}}; }
Info info_exact(std::vector<std::string> v) { return {std::move(v), TQ{}}; }

void dedup(std::vector<std::string>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

Info concat(Info cur, Info n) {
  if (cur.exact && n.exact && cur.exact->size() * n.exact->size() <= kMaxExact) {
    std::vector<std::string> set;
    for (const auto& a : *cur.exact)
      for (const auto& b : *n.exact) set.push_back(a + b);
    dedup(set);
    return {std::move(set), tq_and(std::move(cur.q), std::move(n.q))};
  }
  TQ q = tq_and(tq_and(std::move(cur.q), exact_query(cur.exact)), std::move(n.q));
  if (n.exact && n.exact->size() <= kMaxExact) return {std::move(n.exact), std::move(q)};
  return {std::nullopt, tq_and(std::move(q), exact_query(n.exact))};
}

Info alternate(std::vector<Info> parts) {
  bool all_exact = true;
  for (const Info& p : parts) all_exact = all_exact && p.exact.has_value();
  if (all_exact) {
    std::vector<std::string> set;
    for (const Info& p : parts) set.insert(set.end(), p.exact->begin(), p.exact->end());
    dedup(set);
    if (set.size() <= kMaxExact) return info_exact(std::move(set));
  }
  TQ ors;
  ors.kind = TQ::Or;
  for (Info& p : parts) {
    TQ q = tq_and(std::move(p.q), exact_query(p.exact));
    if (q.kind == TQ::All) return info_all();
    ors.kids.push_back(std::move(q));
  }
  return {std::nullopt, std::move(ors)};
}

// A conservative reader of PCRE syntax: anything it doesn't understand
// becomes "no requirement", which only costs extra candidate reads.
class RegexPlanner {
 public:
  explicit RegexPlanner(std::string_view p) : p_(p) {}
  TQ plan() {
    Info i = alternation();
    if (i_ != p_.size()) return TQ{};  // unbalanced: give up safely
    return tq_and(std::move(i.q), exact_query(i.exact));
  }

 private:
  Info alternation() {
    std::vector<Info> parts{sequence()};
    while (i_ < p_.size() && p_[i_] == '|') {
      ++i_;
      parts.push_back(sequence());
    }
    return parts.size() == 1 ? std::move(parts[0]) : alternate(std::move(parts));
  }

  Info sequence() {
    Info cur = info_exact({""});
    while (i_ < p_.size() && p_[i_] != '|' && p_[i_] != ')') cur = concat(std::move(cur), repeat());
    return cur;
  }

  Info repeat() {
    Info a = atom();
    for (;;) {
      if (i_ >= p_.size()) return a;
      const char c = p_[i_];
      int lo = -1;
      if (c == '*' || c == '?') lo = 0, ++i_;
      else if (c == '+') lo = 1, ++i_;
      else if (c == '{') {
        size_t j = i_ + 1, n = 0;
        bool digits = false;
        while (j < p_.size() && p_[j] >= '0' && p_[j] <= '9') n = n * 10 + size_t(p_[j++] - '0'), digits = true;
        const size_t close = p_.find('}', i_);
        if (!digits || close == std::string_view::npos) return a;  // a literal '{'
        lo = int(std::min<size_t>(n, 2));
        i_ = close + 1;
      } else {
        return a;
      }
      if (i_ < p_.size() && (p_[i_] == '?' || p_[i_] == '+')) ++i_;  // lazy / possessive
      if (lo == 0) a = info_all();
      else a = {std::nullopt, tq_and(std::move(a.q), exact_query(a.exact))};  // at least one copy appears
    }
  }

  Info atom() {
    const char c = p_[i_++];
    switch (c) {
      case '.':
        return info_all();
      case '^': case '$':
        return info_exact({""});
      case '(': {
        bool look = false;
        if (i_ < p_.size() && p_[i_] == '?') {
          ++i_;
          if (i_ < p_.size() && (p_[i_] == '=' || p_[i_] == '!')) look = true, ++i_;
          else if (i_ + 1 < p_.size() && p_[i_] == '<' && (p_[i_ + 1] == '=' || p_[i_ + 1] == '!')) look = true, i_ += 2;
          else if (i_ < p_.size() && (p_[i_] == '<' || p_[i_] == 'P' || p_[i_] == '\'')) {
            const size_t close = p_.find_first_of(">'", i_ + 1);
            i_ = close == std::string_view::npos ? p_.size() : close + 1;
          } else {
            // (?:  (?i)  (?i:  flags: folding already ignores case.
            while (i_ < p_.size() && p_[i_] != ':' && p_[i_] != ')') ++i_;
            if (i_ < p_.size() && p_[i_] == ')') {
              ++i_;
              return info_exact({""});
            }
            if (i_ < p_.size()) ++i_;
          }
        }
        Info in = alternation();
        if (i_ < p_.size() && p_[i_] == ')') ++i_;
        else i_ = p_.size() + 1;  // unbalanced
        return look ? info_exact({""}) : in;
      }
      case '[':
        return char_class();
      case '\\':
        return escape();
      default:
        return info_exact({std::string(1, char(fold(uint8_t(c))))});
    }
  }

  Info escape() {
    if (i_ >= p_.size()) return info_all();
    const char c = p_[i_++];
    switch (c) {
      case 'b': case 'B': case 'A': case 'z': case 'Z': case 'G': case 'K':
        return info_exact({""});
      case 'n': return info_exact({"\n"});
      case 't': return info_exact({"\t"});
      case 'r': return info_exact({"\r"});
      case 'Q': {
        const size_t end = p_.find("\\E", i_);
        std::string lit(p_.substr(i_, end == std::string_view::npos ? std::string_view::npos : end - i_));
        i_ = end == std::string_view::npos ? p_.size() : end + 2;
        for (auto& ch : lit) ch = char(fold(uint8_t(ch)));
        return info_exact({lit});
      }
      case 'x': case 'p': case 'P': case 'N':
        if (i_ < p_.size() && p_[i_] == '{') {
          const size_t close = p_.find('}', i_);
          i_ = close == std::string_view::npos ? p_.size() : close + 1;
        } else if (c == 'x') {
          i_ = std::min(p_.size(), i_ + 2);
        } else if (c != 'N') {
          i_ = std::min(p_.size(), i_ + 1);
        }
        return info_all();
      default:
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return info_all();
        return info_exact({std::string(1, char(fold(uint8_t(c))))});  // escaped punctuation
    }
  }

  // Up to 8 literal members become exact strings; anything else matches any.
  Info char_class() {
    bool negated = false, plain = true;
    if (i_ < p_.size() && p_[i_] == '^') negated = true, ++i_;
    std::vector<std::string> set;
    bool first = true;
    while (i_ < p_.size() && (p_[i_] != ']' || first)) {
      first = false;
      char c = p_[i_++];
      if (c == '[' && i_ < p_.size() && p_[i_] == ':') {
        const size_t close = p_.find(":]", i_);
        i_ = close == std::string_view::npos ? p_.size() : close + 2;
        plain = false;
        continue;
      }
      if (c == '\\') {
        if (i_ >= p_.size()) break;
        const char e = p_[i_++];
        if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '0' && e <= '9')) {
          plain = false;
          continue;
        }
        c = e;
      }
      if (i_ + 1 < p_.size() && p_[i_] == '-' && p_[i_ + 1] != ']') {
        const char hi = p_[i_ + 1];
        i_ += 2;
        if (uint8_t(hi) < uint8_t(c) || uint8_t(hi) - uint8_t(c) > 8) {
          plain = false;
          continue;
        }
        for (int x = uint8_t(c); x <= uint8_t(hi); ++x) set.push_back(std::string(1, char(fold(uint8_t(x)))));
        continue;
      }
      set.push_back(std::string(1, char(fold(uint8_t(c)))));
    }
    if (i_ < p_.size()) ++i_;  // ']'
    dedup(set);
    if (negated || !plain || set.empty() || set.size() > 8) return info_all();
    return info_exact(std::move(set));
  }

  std::string_view p_;
  size_t i_ = 0;
};

// ---------------------------------------------------------------- matching

#if defined(__ARM_NEON)
uint64_t lane_bits(uint8x16_t eq) {
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
}
#endif

// First case-insensitive match of the folded `needle` in [from, end), or npos.
size_t find_ci(std::string_view hay, size_t from, std::string_view needle) {
  const size_t k = needle.size();
  if (k == 0 || hay.size() < k || from > hay.size() - k) return std::string_view::npos;
  const size_t last = hay.size() - k;
  const uint8_t f = uint8_t(needle[0]), l = uint8_t(needle[k - 1]);
  const uint8_t fm = f >= 'a' && f <= 'z' ? 0x20 : 0, lm = l >= 'a' && l <= 'z' ? 0x20 : 0;
  auto at = [&](size_t i) {
    for (size_t j = 0; j < k; ++j)
      if (fold(uint8_t(hay[i + j])) != uint8_t(needle[j])) return false;
    return true;
  };
  size_t i = from;
#if defined(__ARM_NEON)
  const uint8x16_t F = vdupq_n_u8(f), L = vdupq_n_u8(l), FM = vdupq_n_u8(fm), LM = vdupq_n_u8(lm);
  for (; i + 16 <= last + 1; i += 16) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(hay.data() + i);
    const uint8x16_t eq = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(a + k - 1), LM), L));
    uint64_t bits = lane_bits(eq);
    while (bits) {
      const size_t c = i + size_t(__builtin_ctzll(bits) >> 2);
      if (at(c)) return c;
      bits &= ~(uint64_t(0xf) << ((c - i) * 4));
    }
  }
#endif
  for (; i <= last; ++i)
    if ((uint8_t(hay[i]) | fm) == f && (uint8_t(hay[i + k - 1]) | lm) == l && at(i)) return i;
  return std::string_view::npos;
}

// A compiled pattern: literal (smart-case) or regex.
struct Matcher {
  bool regex = false;
  bool caseless = true;
  std::string needle;  // literal; folded when caseless
  std::shared_ptr<const Regex> re;

  bool find(std::string_view s, size_t from, size_t& a, size_t& b) const {
    if (regex) return re->find(s, from, a, b);
    size_t p;
    if (caseless) {
      p = find_ci(s, from, needle);
    } else {
      const void* r = from <= s.size() ? memmem(s.data() + from, s.size() - from, needle.data(), needle.size()) : nullptr;
      p = r ? size_t(static_cast<const char*>(r) - s.data()) : std::string_view::npos;
    }
    if (p == std::string_view::npos) return false;
    a = p, b = p + needle.size();
    return true;
  }
};

bool has_upper(std::string_view s) {
  for (char c : s)
    if (c >= 'A' && c <= 'Z') return true;
  return false;
}

std::string trim_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
  if (line.size() > 200) {
    size_t n = 200;
    while (n > 0 && (uint8_t(line[n]) & 0xc0) == 0x80) --n;  // don't cut a UTF-8 sequence
    line = line.substr(0, n);
  }
  return std::string(line);
}

// Reads one file fresh and collects up to `per_file` matching lines.
bool match_file(const Matcher& m, const std::string& path, size_t per_file, BigVec<char>& buf, FileMatches& out) {
  if (!read_file(path.c_str(), buf, kMaxContentFile * 4) || looks_binary(buf)) return false;
  const std::string_view s(buf.data(), buf.size());
  size_t from = 0, counted = 0, line_no = 1, last_start = std::string_view::npos;
  size_t a, b;
  while (out.lines.size() < per_file && from <= s.size() && m.find(s, from, a, b)) {
    size_t ls = a;
    while (ls > 0 && s[ls - 1] != '\n') --ls;
    const void* nl = std::memchr(s.data() + a, '\n', s.size() - a);
    const size_t le = nl ? size_t(static_cast<const char*>(nl) - s.data()) : s.size();
    if (ls != last_start) {
      for (const char* p = s.data() + counted; (p = static_cast<const char*>(std::memchr(p, '\n', size_t(s.data() + ls - p))));
           ++p)
        ++line_no;
      counted = ls;
      last_start = ls;
      out.lines.push_back({uint32_t(line_no), trim_line(s.substr(ls, le - ls))});
    }
    from = std::max(le + 1, b == a ? a + 1 : b);
  }
  if (out.lines.empty()) return false;
  out.path = path;
  return true;
}

}  // namespace

// ---------------------------------------------------------------- segments

struct ContentIndex::Segment {
  uint64_t id = 0;
  uint32_t ndocs = 0;
  std::shared_ptr<const void> backing;
  size_t bytes = 0;
  Span<uint32_t> key, off, path_off, mtime, by_path;
  Span<uint8_t> post;
  Span<char> paths;
  Span<uint64_t> size;
  Span<int8_t> rank;

  std::string_view path(uint32_t d) const { return {paths.p + path_off[d], path_off[d + 1] - path_off[d]}; }

  // Appends the docs of trigram slot k (ascending) whose follow mask has
  // every bit of `need` to `out`, and their masks to `masks` if given. Dense
  // (bitset) lists carry no masks: they pass every `need`.
  void list(size_t k, std::vector<uint32_t>& out, uint8_t need = 0, std::vector<uint8_t>* masks = nullptr) const {
    const uint32_t a = off[k] & ~kBitset, b = off[k + 1] & ~kBitset;
    const uint8_t* p = post.p + a;
    if (off[k] & kBitset) {
      for (uint32_t w = 0; w < b - a; ++w)
        for (uint8_t x = p[w]; x; x &= uint8_t(x - 1)) {
          out.push_back(w * 8 + uint32_t(__builtin_ctz(x)));
          if (masks) masks->push_back(0x7f);
        }
      return;
    }
    const uint8_t* end = post.p + b;
    uint32_t last = 0, v;
    while (p < end) {
      p = get_varint(p, v);
      last += v;
      const uint8_t m = *p++;
      if ((m & need) != need) continue;
      out.push_back(last);
      if (masks) masks->push_back(m);
    }
  }
  size_t find(uint32_t t) const {
    const uint32_t* it = std::lower_bound(key.p, key.p + key.n, t);
    return it != key.p + key.n && *it == t ? size_t(it - key.p) : SIZE_MAX;
  }
  // Keeps the docs of `acc` (ascending) that varint slot k lists with every
  // bit of `need`. Equivalent to list() + set_intersection, but decodes in
  // place without buffers and stops once the list passes acc's last doc.
  void intersect(size_t k, std::vector<uint32_t>& acc, uint8_t need) const {
    const uint8_t* p = post.p + (off[k] & ~kBitset);
    const uint8_t* end = post.p + (off[k + 1] & ~kBitset);
    const size_t n = acc.size();
    size_t i = 0, w = 0;
    uint32_t last = 0, v;
    while (p < end) {
      p = get_varint(p, v);
      last += v;
      const uint8_t m = *p++;
      while (acc[i] < last)
        if (++i == n) goto done;
      if (acc[i] == last) {
        if ((m & need) == need) acc[w++] = last;
        if (++i == n) break;
      }
    }
  done:
    acc.resize(w);
  }
  size_t list_bytes(size_t k) const { return (off[k + 1] & ~kBitset) - (off[k] & ~kBitset); }
  bool is_bitset(size_t k) const { return off[k] & kBitset; }
  bool has(size_t k, uint32_t d) const {  // bitset lists only
    return (post.p[(off[k] & ~kBitset) + d / 8] >> (d % 8)) & 1;
  }
};

struct SegView {
  std::shared_ptr<const ContentIndex::Segment> seg;
  std::shared_ptr<const std::vector<uint64_t>> dead;
  size_t live = 0;
  bool is_dead(uint32_t d) const { return ((*dead)[d >> 6] >> (d & 63)) & 1; }
};

struct ContentIndex::State {
  std::vector<SegView> segs;
};

namespace {

std::string seg_file(const std::string& dir, uint64_t id, const char* ext) {
  char name[64];
  std::snprintf(name, sizeof name, "/seg-%06llu.%s", static_cast<unsigned long long>(id), ext);
  return dir + name;
}

// The container format of store.cpp, mapped without its read-ahead hint:
// a query touches a few posting lists, not the whole file.
std::shared_ptr<ContentIndex::Segment> map_segment(const std::string& dir, uint64_t id) {
  const std::string file = seg_file(dir, id, "fpc");
  const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st{};
  constexpr size_t kHeader = 8 + 8 + 8 * 8;
  if (fstat(fd, &st) != 0 || size_t(st.st_size) < kHeader + 2 * kFields * 8) {
    close(fd);
    return nullptr;
  }
  const size_t len = size_t(st.st_size);
  void* map = mmap(nullptr, len, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) return nullptr;
  madvise(map, len, MADV_RANDOM);
  auto seg = std::make_shared<ContentIndex::Segment>();
  seg->backing = std::shared_ptr<const void>(map, [len](const void* p) { munmap(const_cast<void*>(p), len); });
  seg->bytes = len;
  seg->id = id;
  const char* base = static_cast<const char*>(map);
  uint64_t count, meta0;
  std::memcpy(&count, base + 8, 8);
  std::memcpy(&meta0, base + 16, 8);
  if (std::memcmp(base, kMagic, 8) != 0 || count != kFields) return nullptr;
  Fields f;
  for (size_t i = 0; i < kFields; ++i) {
    uint64_t o, s;
    std::memcpy(&o, base + kHeader + 16 * i, 8);
    std::memcpy(&s, base + kHeader + 16 * i + 8, 8);
    if (o > len || s > len - o) return nullptr;
    f.f.emplace_back(base + o, size_t(s));
  }
  seg->ndocs = uint32_t(meta0);
  const bool ok = f.get(kKey, seg->key) && f.get(kOff, seg->off) && f.get(kPost, seg->post) &&
                  f.get(kPathOff, seg->path_off) && f.get(kPaths, seg->paths) && f.get(kSize, seg->size) &&
                  f.get(kMtime, seg->mtime) && f.get(kByPath, seg->by_path) && f.get(kRank, seg->rank) &&
                  seg->off.n == seg->key.n + 1 && seg->path_off.n == size_t(seg->ndocs) + 1 &&
                  seg->size.n == seg->ndocs && seg->mtime.n == seg->ndocs && seg->by_path.n == seg->ndocs &&
                  seg->rank.n == seg->ndocs;
  return ok ? seg : nullptr;
}

std::vector<uint64_t> load_dead(const std::string& dir, uint64_t id, uint32_t ndocs) {
  std::vector<uint64_t> dead((ndocs + 63) / 64, 0);
  const int fd = open(seg_file(dir, id, "dead").c_str(), O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    const ssize_t n = read(fd, dead.data(), dead.size() * 8);
    (void)n;
    close(fd);
  }
  return dead;
}

void save_dead(const std::string& dir, uint64_t id, const std::vector<uint64_t>& dead) {
  const std::string file = seg_file(dir, id, "dead"), tmp = file + ".tmp";
  const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  const bool ok = write(fd, dead.data(), dead.size() * 8) == ssize_t(dead.size() * 8);
  close(fd);
  if (!ok || rename(tmp.c_str(), file.c_str()) != 0) unlink(tmp.c_str());
}

size_t live_count(const std::vector<uint64_t>& dead, uint32_t ndocs) {
  size_t n = ndocs;
  for (uint64_t w : dead) n -= size_t(__builtin_popcountll(w));
  return n;
}

struct DocMeta {
  std::string_view path;
  uint64_t size;
  uint32_t mtime;
  int8_t rank;
};

// Encodes postings (a bitset when dense, else delta varints each followed
// by the doc's follow mask) and writes a segment. `next` fills one
// trigram's ascending doc ids and their masks, and the trigram, ascending,
// until it returns false.
template <typename Next>
// Writes a segment, streaming the postings to the file as `next` produces
// them (they are the bulk of it), so a big segment never sits in memory.
bool write_segment(const std::string& dir, uint64_t id, const std::vector<DocMeta>& docs, Next&& next) {
  const uint32_t ndocs = uint32_t(docs.size());
  FieldWriter w(seg_file(dir, id, "fpc"), kMagic, kFields);
  BigVec<uint32_t> keys, off;
  std::span<const uint32_t> list;
  std::span<const uint8_t> masks;
  std::vector<uint8_t> out;
  uint64_t post = 0;  // bytes written so far
  w.begin(kPost);
  uint32_t t;
  while (next(list, masks, t)) {
    if (list.empty()) continue;
    if (post >= kBitset) return false;  // offsets must fit in 31 bits
    keys.push_back(t);
    out.clear();
    if (list.size() * 8 > ndocs) {
      off.push_back(uint32_t(post) | kBitset);
      out.assign((ndocs + 7) / 8, 0);
      for (uint32_t d : list) out[d / 8] |= uint8_t(1u << (d % 8));
    } else {
      off.push_back(uint32_t(post));
      uint32_t last = 0;
      for (size_t i = 0; i < list.size(); ++i) {
        put_varint(out, list[i] - last);
        out.push_back(masks[i] & 0x7f);
        last = list[i];
      }
    }
    w.append(out.data(), out.size());
    post += out.size();
  }
  off.push_back(uint32_t(post));
  static const char pad[8] = {};
  w.append(pad, sizeof pad);  // varint reads never run off the end
  w.end();
  BigVec<char> paths;
  BigVec<uint32_t> path_off{0}, mtime, by_path(ndocs);
  BigVec<uint64_t> size;
  BigVec<int8_t> rank;
  for (const DocMeta& d : docs) {
    paths.insert(paths.end(), d.path.begin(), d.path.end());
    path_off.push_back(uint32_t(paths.size()));
    size.push_back(d.size);
    mtime.push_back(d.mtime);
    rank.push_back(d.rank);
  }
  for (uint32_t i = 0; i < ndocs; ++i) by_path[i] = i;
  std::sort(by_path.begin(), by_path.end(), [&](uint32_t a, uint32_t b) { return docs[a].path < docs[b].path; });
  w.write(kKey, keys.data(), keys.size() * sizeof keys[0]);
  w.write(kOff, off.data(), off.size() * sizeof off[0]);
  w.write(kPathOff, path_off.data(), path_off.size() * sizeof path_off[0]);
  w.write(kPaths, paths.data(), paths.size());
  w.write(kSize, size.data(), size.size() * sizeof size[0]);
  w.write(kMtime, mtime.data(), mtime.size() * sizeof mtime[0]);
  w.write(kByPath, by_path.data(), by_path.size() * sizeof by_path[0]);
  w.write(kRank, rank.data(), rank.size());
  std::array<uint64_t, 8> meta{};
  meta[0] = ndocs;
  return w.finish(meta);
}

void set_background(bool background) {
  pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
  // Never download a cloud placeholder just to index it.
  setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
}

// Runs fn(i) for i in [0, n) on `threads` threads, claiming small chunks.
template <typename Fn>
void parallel(size_t n, unsigned threads, bool background, size_t chunk, Fn&& fn) {
  std::atomic<size_t> next{0};
  auto work = [&](unsigned t) {
    set_background(background);
    for (size_t a; (a = next.fetch_add(chunk)) < n;)
      for (size_t i = a; i < std::min(n, a + chunk); ++i) fn(t, i);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
  work(0);
  for (auto& th : pool) th.join();
}

}  // namespace

// ---------------------------------------------------------------- index

ContentIndex::ContentIndex(std::string dir, std::string home, unsigned readers)
    : dir_(std::move(dir)), home_(std::move(home)), state_(std::make_shared<State>()),
      readers_(std::make_unique<Pool>(std::max(1u, readers))) {
  while (home_.size() > 1 && home_.back() == '/') home_.pop_back();
}

ContentIndex::~ContentIndex() = default;

std::shared_ptr<const ContentIndex::State> ContentIndex::state() const {
  std::lock_guard lk(m_);
  return state_;
}

void ContentIndex::publish(std::shared_ptr<const State> s) {
  std::lock_guard lk(m_);
  state_ = std::move(s);
}

uint64_t ContentIndex::next_id() { return next_id_.fetch_add(1); }

void ContentIndex::save_manifest(const State& s) const {
  std::string text;
  for (const SegView& v : s.segs) text += std::to_string(v.seg->id) + "\n";
  const std::string file = dir_ + "/manifest", tmp = file + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) == 0 && ok) rename(tmp.c_str(), file.c_str());
  else unlink(tmp.c_str());
}

bool ContentIndex::open() {
  std::lock_guard w(write_m_);
  mkdir(dir_.c_str(), 0755);
  auto s = std::make_shared<State>();
  std::unordered_set<std::string> keep;
  if (FILE* f = std::fopen((dir_ + "/manifest").c_str(), "r")) {
    unsigned long long id;
    while (std::fscanf(f, "%llu", &id) == 1) {
      auto seg = map_segment(dir_, id);
      if (!seg) continue;
      auto dead = std::make_shared<std::vector<uint64_t>>(load_dead(dir_, id, seg->ndocs));
      const size_t live = live_count(*dead, seg->ndocs);
      s->segs.push_back({std::move(seg), std::move(dead), live});
      keep.insert(seg_file("", id, "fpc").substr(1));
      keep.insert(seg_file("", id, "dead").substr(1));
      next_id_ = std::max<uint64_t>(next_id_, id + 1);
    }
    std::fclose(f);
  }
  // Segments a crashed build left behind are garbage.
  if (DIR* d = opendir(dir_.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name.starts_with("seg-") && !keep.contains(name)) unlink((dir_ + "/" + name).c_str());
    }
    closedir(d);
  }
  const bool any = !s->segs.empty();
  publish(std::move(s));
  return any;
}

bool ContentIndex::covers(std::string_view path) const { return path_in_scope(std::string(path) + "/x", home_); }

size_t ContentIndex::docs() const {
  size_t n = 0;
  for (const SegView& v : state()->segs) n += v.live;
  return n;
}

size_t ContentIndex::bytes() const {
  size_t n = 0;
  for (const SegView& v : state()->segs) n += v.seg->bytes;
  return n;
}

void ContentIndex::add_docs(std::vector<DocIn>& docs, bool background) {
  if (docs.empty()) return;
  const unsigned threads = std::min(12u, std::max(1u, std::thread::hardware_concurrency()));
  BigVec<uint32_t> count(size_t(1) << 22, 0);  // reused per shard: trigram -> slot, then count
  for (size_t b0 = 0; b0 < docs.size();) {
    size_t b1 = b0;
    for (uint64_t bytes = 0; b1 < docs.size() && (bytes < kBatchBytes || b1 == b0); ++b1) bytes += docs[b1].size;
    const size_t n = b1 - b0;
    // Read and extract in parallel; each thread keeps its own trigram runs.
    struct Local {
      std::vector<uint32_t> scratch;
      BigVec<char> buf;
      BigVec<uint32_t> flat;  // mapped directly, so freeing it returns the memory
    };
    std::vector<Local> locals(threads);
    struct Run {
      uint32_t thread = 0, start = 0, len = 0;
      int8_t rank = kNotText;
      uint64_t size = 0;
      uint32_t mtime = 0;
    };
    std::vector<Run> runs(n);
    parallel(n, threads, background, 16, [&](unsigned t, size_t i) {
      Local& L = locals[t];
      const DocIn& d = docs[b0 + i];
      Run& r = runs[i];
      r.thread = t;
      r.start = uint32_t(L.flat.size());
      r.size = d.size, r.mtime = d.mtime;
      struct stat st{};
      if (!read_file(std::string(d.path).c_str(), L.buf, kMaxContentFile + 1, &st)) return;
      r.size = uint64_t(st.st_size), r.mtime = uint32_t(st.st_mtimespec.tv_sec);
      if (L.buf.size() > kMaxContentFile || looks_binary(L.buf)) return;
      trigrams(L.buf.data(), L.buf.size(), L.scratch, L.flat);
      r.len = uint32_t(L.flat.size() - r.start);
      r.rank = doc_rank(d.path);
    });
    for (Local& L : locals) std::vector<uint32_t>().swap(L.scratch), BigVec<char>().swap(L.buf);

    // Counting sort of (trigram, doc) pairs, a quarter of the trigram space
    // at a time: each doc's trigrams are sorted, so a cursor per doc walks
    // through them shard by shard, and only one shard's pairs are held.
    constexpr uint32_t kShardBits = 22, kShards = 1u << (24 - kShardBits);
    auto tris = [&](size_t i) {
      const Run& r = runs[i];
      return std::pair(locals[r.thread].flat.data() + r.start, r.len);
    };
    std::vector<uint32_t> cursor(n, 0), shard_end(n, 0), keys;
    BigVec<uint64_t> start;
    BigVec<uint32_t> raw;
    BigVec<uint8_t> raw_mask;
    auto load_shard = [&](uint32_t sh) {
      std::fill(count.begin(), count.end(), 0);
      for (size_t i = 0; i < n; ++i) {
        auto [p, len] = tris(i);
        uint32_t j = cursor[i];
        for (; j < len && (p[j] >> (8 + kShardBits)) == sh; ++j) ++count[(p[j] >> 8) & ((1u << kShardBits) - 1)];
        shard_end[i] = j;
      }
      keys.clear();
      start.assign(1, 0);
      for (uint32_t t = 0; t < (1u << kShardBits); ++t) {
        if (!count[t]) continue;
        start.push_back(start.back() + count[t]);
        count[t] = uint32_t(keys.size());
        keys.push_back(sh << kShardBits | t);
      }
      raw.assign(start.back(), 0);
      raw_mask.assign(start.back(), 0);
      for (size_t i = 0; i < n; ++i) {
        auto [p, len] = tris(i);
        for (uint32_t j = cursor[i]; j < shard_end[i]; ++j) {
          const uint64_t a = start[count[(p[j] >> 8) & ((1u << kShardBits) - 1)]]++;
          raw[a] = uint32_t(i);
          raw_mask[a] = uint8_t(p[j]);
        }
        cursor[i] = shard_end[i];
      }
      // Insertion cursors now hold bucket ends; shift to recover starts.
      for (size_t k = start.size() - 1; k > 0; --k) start[k] = start[k - 1];
      start[0] = 0;
    };
    std::vector<DocMeta> meta(n);
    for (size_t i = 0; i < n; ++i) meta[i] = {docs[b0 + i].path, runs[i].size, runs[i].mtime, runs[i].rank};
    const uint64_t id = next_id();
    size_t k = 0;
    uint32_t shard = 0;
    load_shard(0);
    const bool ok = write_segment(dir_, id, meta, [&](std::span<const uint32_t>& list, std::span<const uint8_t>& masks, uint32_t& t) {
      while (k >= keys.size()) {
        if (++shard >= kShards) return false;
        load_shard(shard);
        k = 0;
      }
      t = keys[k];
      list = std::span<const uint32_t>(raw.data(), raw.size()).subspan(start[k], start[k + 1] - start[k]);
      masks = std::span<const uint8_t>(raw_mask.data(), raw_mask.size()).subspan(start[k], start[k + 1] - start[k]);
      ++k;
      return true;
    });
    for (Local& L : locals) BigVec<uint32_t>().swap(L.flat);
    if (ok)
      if (auto seg = map_segment(dir_, id)) {
        auto next = std::make_shared<State>(*state());
        auto dead = std::make_shared<std::vector<uint64_t>>((seg->ndocs + 63) / 64, 0);
        next->segs.push_back({std::move(seg), std::move(dead), n});
        save_manifest(*next);
        publish(std::move(next));
      }
    b0 = b1;
  }
}

// Merges segments so a query visits few of them: kMergeTier segments of
// one size tier become one (updates never pile up small segments), and
// then the smallest are merged while there are more than kMaxSegments.
void ContentIndex::maybe_merge() {
  for (;;) {
    auto cur = state();
    std::vector<std::vector<size_t>> tiers(64);
    for (size_t i = 0; i < cur->segs.size(); ++i) {
      const size_t plen = std::max<size_t>(1, cur->segs[i].seg->post.n);
      tiers[size_t(std::log(double(plen)) / std::log(4.0))].push_back(i);
    }
    std::vector<size_t> group;
    for (auto& t : tiers) {
      if (t.size() < kMergeTier) continue;
      uint64_t bytes = 0;
      for (size_t j = 0; j < kMergeTier; ++j) bytes += cur->segs[t[j]].seg->post.n;
      if (bytes <= kMergeCap) {
        group.assign(t.begin(), t.begin() + kMergeTier);
        break;
      }
    }
    if (group.empty() && cur->segs.size() > kMaxSegments) {
      std::vector<size_t> by_size(cur->segs.size());
      for (size_t i = 0; i < by_size.size(); ++i) by_size[i] = i;
      std::sort(by_size.begin(), by_size.end(),
                [&](size_t a, size_t b) { return cur->segs[a].seg->post.n < cur->segs[b].seg->post.n; });
      uint64_t bytes = 0;
      for (size_t i : by_size) {
        if (group.size() >= 2 && bytes + cur->segs[i].seg->post.n > kMergeCap) break;
        bytes += cur->segs[i].seg->post.n;
        group.push_back(i);
      }
      if (group.size() < 2) group.clear();
    }
    if (group.empty()) return;
    std::vector<DocMeta> meta;
    std::vector<std::vector<uint32_t>> remap;
    for (size_t gi : group) {
      const SegView& v = cur->segs[gi];
      std::vector<uint32_t> r(v.seg->ndocs, UINT32_MAX);
      for (uint32_t d = 0; d < v.seg->ndocs; ++d) {
        if (v.is_dead(d)) continue;
        r[d] = uint32_t(meta.size());
        meta.push_back({v.seg->path(d), v.seg->size[d], v.seg->mtime[d], v.seg->rank[d]});
      }
      remap.push_back(std::move(r));
    }
    std::vector<size_t> pos(group.size(), 0);
    std::vector<uint32_t> part;
    std::vector<uint8_t> part_masks;
    const uint64_t id = next_id();
    std::vector<uint32_t> list;
    std::vector<uint8_t> masks;
    const bool ok = !meta.empty() && write_segment(dir_, id, meta, [&](std::span<const uint32_t>& view, std::span<const uint8_t>& mask_view, uint32_t& t) {
      list.clear();
      masks.clear();
      for (;;) {
        bool any = false;
        t = UINT32_MAX;
        for (size_t g = 0; g < group.size(); ++g) {
          const Segment& s = *cur->segs[group[g]].seg;
          if (pos[g] < s.key.n) any = true, t = std::min(t, s.key[pos[g]]);
        }
        if (!any) return false;
        for (size_t g = 0; g < group.size(); ++g) {
          const Segment& s = *cur->segs[group[g]].seg;
          if (pos[g] >= s.key.n || s.key[pos[g]] != t) continue;
          part.clear();
          part_masks.clear();
          s.list(pos[g]++, part, 0, &part_masks);
          for (size_t i = 0; i < part.size(); ++i)
            if (remap[g][part[i]] != UINT32_MAX) list.push_back(remap[g][part[i]]), masks.push_back(part_masks[i]);
        }
        if (!list.empty()) {
          view = list;
          mask_view = masks;
          return true;
        }
      }
    });
    auto seg = ok ? map_segment(dir_, id) : nullptr;
    // Writers are serialized, so nothing was killed in the group meanwhile.
    auto next = std::make_shared<State>();
    std::vector<uint64_t> gone;
    for (size_t i = 0; i < cur->segs.size(); ++i) {
      if (std::find(group.begin(), group.end(), i) == group.end()) next->segs.push_back(cur->segs[i]);
      else gone.push_back(cur->segs[i].seg->id);
    }
    if (seg) {
      auto dead = std::make_shared<std::vector<uint64_t>>((seg->ndocs + 63) / 64, 0);
      const size_t live = seg->ndocs;
      next->segs.push_back({std::move(seg), std::move(dead), live});
    } else if (!meta.empty()) {
      return;  // could not write: keep what we have
    }
    save_manifest(*next);
    publish(std::move(next));
    for (uint64_t g : gone) unlink(seg_file(dir_, g, "fpc").c_str()), unlink(seg_file(dir_, g, "dead").c_str());
  }
}

void ContentIndex::sync(const Index& ix, bool background, ScanProgress* progress) {
  std::lock_guard w(write_m_);
  // Wanted: eligible files under home, from the file index. Paths are
  // built in one reused buffer (depth-first), names read without copies.
  PathArena arena;
  std::vector<DocIn> want;
  const uint32_t home = ix.lookup(home_);
  if (home != UINT32_MAX && ix.is_dir(home)) {
    std::string path = home_;
    char buf[256 + 32];
    std::vector<std::pair<uint32_t, size_t>> stack{{home, path.size()}};  // directory, its path length
    std::vector<std::string> names{""};
    while (!stack.empty()) {
      const auto [d, len] = stack.back();
      stack.pop_back();
      path.resize(len);
      if (d != home) path += '/', path += names.back();
      names.pop_back();
      const size_t base = path.size();
      for (uint32_t c : ix.kids(d)) {
        if (ix.flags[c] & kFlagLink) continue;
        const std::string_view name = ix.name_view(c, buf);
        path.resize(base);
        path += '/';
        path += name;
        if (ix.is_dir(c)) {
          if (!skip_dir(std::string_view(path).substr(home_.size() + 1), name)) {
            stack.push_back({c, base});
            names.emplace_back(name);
          }
        } else if (file_ok(name, ix.size(c))) {
          want.push_back({arena.add(path), ix.size(c), ix.mtime[c]});
        }
      }
    }
  }
  std::sort(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path < b.path; });

  // Merge live docs in path order to diff against the sorted wanted list.
  auto cur = state();
  struct Held {
    std::string_view path;
    uint32_t seg, doc;
  };
  auto later = [](const Held& a, const Held& b) { return a.path > b.path; };
  std::priority_queue<Held, std::vector<Held>, decltype(later)> held(later);
  std::vector<uint32_t> positions(cur->segs.size(), 0);
  auto advance = [&](uint32_t si) {
    const SegView& v = cur->segs[si];
    uint32_t& k = positions[si];
    while (k < v.seg->ndocs) {
      const uint32_t d = v.seg->by_path[k++];
      if (!v.is_dead(d)) {
        held.push({v.seg->path(d), si, d});
        break;
      }
    }
  };
  for (uint32_t si = 0; si < cur->segs.size(); ++si) advance(si);
  size_t todo = 0;  // want[0, todo): the docs to (re)read, compacted in place
  std::vector<std::vector<uint64_t>> dead(cur->segs.size());
  std::vector<bool> touched(cur->segs.size(), false);
  auto kill = [&](const Held& h) {
    if (!touched[h.seg]) dead[h.seg] = *cur->segs[h.seg].dead, touched[h.seg] = true;
    dead[h.seg][h.doc >> 6] |= uint64_t(1) << (h.doc & 63);
  };
  size_t i = 0;
  while (i < want.size() || !held.empty()) {
    if (held.empty() || (i < want.size() && want[i].path < held.top().path)) {
      want[todo++] = want[i++];
      continue;
    }
    const Held h = held.top();
    held.pop();
    advance(h.seg);
    if (i >= want.size() || h.path < want[i].path) {
      kill(h);
    } else {
      const Segment& s = *cur->segs[h.seg].seg;
      // Not-text docs keep their stat from when they were read; recheck
      // them only when the file index reports a change.
      if (s.size[h.doc] != want[i].size || s.mtime[h.doc] != want[i].mtime) {
        kill(h);
        want[todo++] = want[i];
      }
      ++i;
    }
  }
  if (std::any_of(touched.begin(), touched.end(), [](bool t) { return t; })) {
    auto next = std::make_shared<State>(*cur);
    for (size_t si = 0; si < touched.size(); ++si) {
      if (!touched[si]) continue;
      save_dead(dir_, next->segs[si].seg->id, dead[si]);
      next->segs[si].live = live_count(dead[si], next->segs[si].seg->ndocs);
      next->segs[si].dead = std::make_shared<std::vector<uint64_t>>(std::move(dead[si]));
    }
    publish(std::move(next));
  }
  cur.reset();
  if (progress) progress->source_files.store(todo, std::memory_order_relaxed);
  want.resize(todo);
  add_docs(want, background);
  std::vector<DocIn>().swap(want);
  arena.clear();
  maybe_merge();
}

void ContentIndex::update_paths(const std::vector<std::string>& dirs, const std::vector<std::string>& trees) {
  std::lock_guard w(write_m_);
  PathArena arena;
  std::vector<DocIn> want;
  std::vector<std::pair<std::string, bool>> areas;  // (folder, whole subtree)
  for (const auto& d : dirs) areas.push_back({d, false});
  for (const auto& d : trees) areas.push_back({d, true});
  // What's on disk now.
  for (auto& [dir, recursive] : areas) {
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (!path_in_scope(dir + "/x", home_)) continue;
    std::vector<std::string> stack{dir};
    while (!stack.empty()) {
      const std::string d = std::move(stack.back());
      stack.pop_back();
      DIR* h = opendir(d.c_str());
      if (!h) continue;
      while (dirent* e = readdir(h)) {
        const std::string_view name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string p = d + "/" + std::string(name);
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
          if (recursive && !skip_dir(std::string_view(p).substr(home_.size() + 1), name)) stack.push_back(p);
        } else if (S_ISREG(st.st_mode) && file_ok(name, uint64_t(st.st_size))) {
          want.push_back({arena.add(p), uint64_t(st.st_size), uint32_t(st.st_mtimespec.tv_sec)});
        }
      }
      closedir(h);
    }
  }
  std::sort(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path < b.path; });
  want.erase(std::unique(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path == b.path; }),
             want.end());
  // What we hold there: direct children (or the whole subtree).
  auto cur = state();
  std::vector<std::vector<uint64_t>> dead(cur->segs.size());
  std::vector<bool> touched(cur->segs.size(), false);
  std::vector<bool> held(want.size(), false);
  for (uint32_t si = 0; si < cur->segs.size(); ++si) {
    const SegView& v = cur->segs[si];
    const Segment& s = *v.seg;
    for (const auto& [dir, recursive] : areas) {
      const std::string lo = dir + "/";
      const uint32_t* bp = s.by_path.p;
      const uint32_t* it = std::lower_bound(bp, bp + s.ndocs, lo, [&](uint32_t d, const std::string& x) { return s.path(d) < x; });
      for (; it != bp + s.ndocs && s.path(*it).starts_with(lo); ++it) {
        const uint32_t d = *it;
        const std::string_view p = s.path(d);
        if (!recursive && p.find('/', lo.size()) != std::string_view::npos) continue;
        if (v.is_dead(d)) continue;
        const auto w = std::lower_bound(want.begin(), want.end(), p, [](const DocIn& a, std::string_view x) { return a.path < x; });
        if (w != want.end() && w->path == p && w->size == s.size[d] && w->mtime == s.mtime[d]) {
          held[size_t(w - want.begin())] = true;
          continue;
        }
        if (!touched[si]) dead[si] = *v.dead, touched[si] = true;
        dead[si][d >> 6] |= uint64_t(1) << (d & 63);
      }
    }
  }
  std::vector<DocIn> todo;
  for (size_t k = 0; k < want.size(); ++k)
    if (!held[k]) todo.push_back(std::move(want[k]));
  if (std::any_of(touched.begin(), touched.end(), [](bool t) { return t; })) {
    auto next = std::make_shared<State>(*cur);
    for (size_t si = 0; si < touched.size(); ++si) {
      if (!touched[si]) continue;
      save_dead(dir_, next->segs[si].seg->id, dead[si]);
      next->segs[si].live = live_count(dead[si], next->segs[si].seg->ndocs);
      next->segs[si].dead = std::make_shared<std::vector<uint64_t>>(std::move(dead[si]));
    }
    publish(std::move(next));
  }
  cur.reset();
  add_docs(todo, false);
  maybe_merge();
}

// ---------------------------------------------------------------- grep

namespace {

// Docs of segment `s` that may satisfy `q`; nullopt means every doc. Lists
// are applied smallest first, and a large varint list is skipped once the
// candidates are few: a superset only costs a read, decoding costs more.
std::optional<std::vector<uint32_t>> eval(const ContentIndex::Segment& s, const TQ& q) {
  switch (q.kind) {
    case TQ::All:
      return std::nullopt;
    case TQ::Tri: {
      const size_t k = s.find(q.tri);
      std::vector<uint32_t> out;
      if (k != SIZE_MAX) s.list(k, out, q.need);
      return out;
    }
    case TQ::Or: {
      std::vector<uint32_t> acc;
      for (const TQ& c : q.kids) {
        auto r = eval(s, c);
        if (!r) return std::nullopt;
        acc.insert(acc.end(), r->begin(), r->end());
      }
      std::sort(acc.begin(), acc.end());
      acc.erase(std::unique(acc.begin(), acc.end()), acc.end());
      return acc;
    }
    case TQ::And:
      break;
  }
  std::vector<size_t> slots;
  std::vector<const TQ*> others;
  std::vector<std::pair<size_t, uint8_t>> found;
  for (const TQ& c : q.kids) {
    if (c.kind == TQ::Tri) {
      const size_t k = s.find(c.tri);
      if (k == SIZE_MAX) return std::vector<uint32_t>{};
      found.push_back({k, c.need});
    } else if (c.kind != TQ::All) {
      others.push_back(&c);
    }
  }
  auto need_of = [&](size_t k) {
    for (auto& f : found)
      if (f.first == k) return f.second;
    return uint8_t(0);
  };
  for (auto& f : found) slots.push_back(f.first);
  std::sort(slots.begin(), slots.end(), [&](size_t a, size_t b) {
    // Bitsets are cheap to test but costly to enumerate: start from a list.
    const size_t ca = s.is_bitset(a) ? SIZE_MAX / 2 + s.list_bytes(a) : s.list_bytes(a);
    const size_t cb = s.is_bitset(b) ? SIZE_MAX / 2 + s.list_bytes(b) : s.list_bytes(b);
    return ca < cb;
  });
  std::optional<std::vector<uint32_t>> acc;
  for (size_t k : slots) {
    if (!acc) {
      acc.emplace();
      s.list(k, *acc, need_of(k));
      continue;
    }
    if (acc->empty()) break;
    if (s.is_bitset(k)) {
      std::erase_if(*acc, [&](uint32_t d) { return !s.has(k, d); });
      continue;
    }
    if (acc->size() <= 4 && s.list_bytes(k) > 4096) continue;
    s.intersect(k, *acc, need_of(k));
  }
  for (const TQ* c : others) {
    if (acc && acc->empty()) break;
    auto r = eval(s, *c);
    if (!r) continue;
    if (!acc) {
      acc = std::move(r);
      continue;
    }
    std::vector<uint32_t> out;
    std::set_intersection(acc->begin(), acc->end(), r->begin(), r->end(), std::back_inserter(out));
    *acc = std::move(out);
  }
  return acc;
}

bool make_matcher(const GrepOptions& o, Matcher& m, std::string& error) {
  m.regex = o.regex;
  m.caseless = !has_upper(o.pattern);  // smart case
  if (o.pattern.empty()) {
    error = "empty pattern";
    return false;
  }
  if (o.regex) {
    m.re = Regex::compile(o.pattern, m.caseless, true, error);
    return m.re != nullptr;
  }
  m.needle = o.pattern;
  if (m.caseless)
    for (auto& c : m.needle) c = char(fold(uint8_t(c)));
  return true;
}

// Reads `paths` in order on the pool, until `limit` files matched or the
// budget is spent. Threads claim paths in order, so the result is exactly
// the first matching files of the read prefix.
template <typename PathAt>
GrepResult verify(Pool& pool, const Matcher& m, const GrepOptions& o, size_t path_count, PathAt path_at) {
  GrepResult r;
  r.candidates = path_count;
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0}, found{0}, read{0};
  std::atomic<bool> out_of_time{false};
  std::mutex mu;
  std::vector<std::pair<size_t, FileMatches>> hits;
  const size_t workers = pool.size();
  pool.run(workers, [&](size_t) {
    BigVec<char> buf;
    setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
    for (;;) {
      if (found.load(std::memory_order_relaxed) >= o.limit) return;
      if (o.budget_ms > 0 &&
          std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(o.budget_ms)) {
        out_of_time = true;
        return;
      }
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= path_count) return;
      read.fetch_add(1, std::memory_order_relaxed);
      FileMatches fm;
      if (!match_file(m, std::string(path_at(i)), std::max<size_t>(1, o.per_file), buf, fm)) continue;
      found.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard lk(mu);
      hits.push_back({i, std::move(fm)});
    }
  });
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  if (hits.size() > o.limit) hits.resize(o.limit);
  for (auto& h : hits) r.files.push_back(std::move(h.second));
  r.read = read.load();
  r.complete = !out_of_time || r.files.size() >= o.limit;
  return r;
}

}  // namespace

GrepResult ContentIndex::grep(const GrepOptions& o) const {
  GrepResult r;
  Matcher m;
  if (!make_matcher(o, m, r.error)) return r;
  const TQ plan = o.regex ? RegexPlanner(o.pattern).plan() : literal_plan(o.pattern);
  auto st = state();
  std::string scope = o.scope;
  while (scope.size() > 1 && scope.back() == '/') scope.pop_back();
  const std::string lo = scope.empty() ? std::string() : scope + "/";
  struct Cand {
    std::string_view path;
    int8_t rank;
    uint32_t mtime;
  };
  // Candidates per segment, in parallel on the reader pool.
  std::vector<std::vector<Cand>> per(st->segs.size());
  std::lock_guard rl(read_m_);
  readers_->run(st->segs.size(), [&](size_t si) {
    const SegView& v = st->segs[si];
    const Segment& s = *v.seg;
    auto take = [&](uint32_t d) {
      if (v.is_dead(d) || s.rank[d] == kNotText) return;
      const std::string_view p = s.path(d);
      if (!lo.empty() && !p.starts_with(lo)) return;
      if (o.keep && !o.keep(p)) return;
      per[si].push_back({p, s.rank[d], s.mtime[d]});
    };
    if (auto ids = eval(s, plan)) {
      for (uint32_t d : *ids) take(d);
      return;
    }
    if (lo.empty()) {
      for (uint32_t d = 0; d < s.ndocs; ++d) take(d);
      return;
    }
    // Every doc in scope: the path-sorted permutation finds the range.
    const uint32_t* bp = s.by_path.p;
    const uint32_t* it = std::lower_bound(bp, bp + s.ndocs, lo, [&](uint32_t d, const std::string& x) { return s.path(d) < x; });
    for (; it != bp + s.ndocs && s.path(*it).starts_with(lo); ++it) take(*it);
  });
  size_t candidate_count = 0;
  for (const auto& p : per) candidate_count += p.size();
  std::vector<Cand> cands;
  cands.reserve(candidate_count);
  for (auto& p : per) {
    cands.insert(cands.end(), p.begin(), p.end());
    std::vector<Cand>().swap(p);
  }
  // Your files before dot-folders and logs, then the most recently changed.
  std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
    return a.rank != b.rank ? a.rank > b.rank : a.mtime != b.mtime ? a.mtime > b.mtime : a.path < b.path;
  });
  return verify(*readers_, m, o, cands.size(), [&](size_t i) { return cands[i].path; });
}

GrepResult ContentIndex::grep_files(const GrepOptions& o, const std::vector<std::string>& paths) const {
  GrepResult r;
  Matcher m;
  if (!make_matcher(o, m, r.error)) return r;
  std::vector<std::string_view> v;
  for (const auto& p : paths)
    if (!o.keep || o.keep(p)) v.push_back(p);
  std::lock_guard rl(read_m_);
  return verify(*readers_, m, o, v.size(), [&](size_t i) { return v[i]; });
}

}  // namespace fplussearch
