#include "store.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace fplussearch {

namespace {

// Header, then (offset, size) per field, then the fields, each 64-byte aligned.
struct Header {
  char magic[8];
  uint64_t count;
  uint64_t meta[8];
};

uint64_t align64(uint64_t v) { return (v + 63) & ~uint64_t(63); }

}  // namespace

MapChunk map_chunk() {
  void* p = mmap(nullptr, kMapChunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  return MapChunk(p);
}

uint32_t Interner::intern(std::string_view s, uint32_t hash) {
  if (slots_.empty()) slots_.assign(size_t(1) << 20, 0);
  const size_t mask = slots_.size() - 1;
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    const uint32_t slot = slots_[i];
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

void Interner::insert(uint32_t id) {
  const size_t mask = slots_.size() - 1;
  size_t i = hash_[id] & mask;
  while (slots_[i]) i = (i + 1) & mask;
  slots_[i] = id + 1;
}

void Interner::grow() {
  slots_.assign(slots_.size() * 2, 0);
  for (uint32_t id = 0; id < ptr_.size(); ++id) insert(id);
}

bool Fields::section(size_t first, Section& s, bool hex) const {
  s.hex = hex;
  return get(first + 0, s.bytes) && get(first + 1, s.block_off) && get(first + 2, s.block_first) &&
         get(first + 3, s.name_off) && get(first + 4, s.counts) && get(first + 5, s.overflow) &&
         get(first + 6, s.mask) && s.block_off.n > 0 && s.block_first.n == s.block_off.n &&
         s.counts.n == s.name_off.n && s.mask.n == s.name_off.n;
}

bool save_fields(const std::string& file, const char (&magic)[8], const std::array<uint64_t, 8>& meta,
                 const std::vector<Blob>& fields) {
  Header h{};
  std::memcpy(h.magic, magic, sizeof h.magic);
  h.count = fields.size();
  std::memcpy(h.meta, meta.data(), sizeof h.meta);
  std::vector<uint64_t> table(2 * fields.size());
  uint64_t off = align64(sizeof h + table.size() * sizeof(uint64_t));
  for (size_t i = 0; i < fields.size(); ++i) {
    table[2 * i] = off;
    table[2 * i + 1] = fields[i].size;
    off = align64(off + fields[i].size);
  }
  // A unique temporary per writer: concurrent builds (a CLI --reindex while
  // the TUI refreshes) must never write into a file another one publishes.
  std::string tmp = file + ".XXXXXX";
  const int fd = mkstemp(tmp.data());
  if (fd < 0) return false;
  fchmod(fd, 0644);
  FILE* f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmp.c_str());
    return false;
  }
  static const char zeros[64] = {};
  bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
            std::fwrite(table.data(), sizeof(uint64_t), table.size(), f) == table.size();
  uint64_t pos = sizeof h + table.size() * sizeof(uint64_t);
  for (size_t i = 0; i < fields.size() && ok; ++i) {
    ok = std::fwrite(zeros, 1, table[2 * i] - pos, f) == table[2 * i] - pos &&
         std::fwrite(fields[i].data, 1, fields[i].size, f) == fields[i].size;
    pos = table[2 * i] + fields[i].size;
  }
  ok &= std::fclose(f) == 0;
  if (!ok || std::rename(tmp.c_str(), file.c_str()) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

bool map_fields(const std::string& file, const char (&magic)[8], size_t count, Fields& out) {
  const int fd = open(file.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st{};
  const size_t table_end = sizeof(Header) + 2 * count * sizeof(uint64_t);
  if (fstat(fd, &st) != 0 || size_t(st.st_size) < table_end) {
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
  if (std::memcmp(h.magic, magic, sizeof h.magic) != 0 || h.count != count) return false;
  Fields r;
  std::memcpy(r.meta.data(), h.meta, sizeof h.meta);
  for (size_t i = 0; i < count; ++i) {
    uint64_t off, size;
    std::memcpy(&off, base + sizeof h + 2 * i * sizeof(uint64_t), sizeof off);
    std::memcpy(&size, base + sizeof h + (2 * i + 1) * sizeof(uint64_t), sizeof size);
    if (off > len || size > len - off) return false;
    r.f.emplace_back(base + off, size_t(size));
  }
  r.backing = std::move(backing);
  r.bytes = len;
  out = std::move(r);
  return true;
}

FieldWriter::FieldWriter(std::string file, const char (&magic)[8], size_t count)
    : file_(std::move(file)), table_(2 * count, 0) {
  std::memcpy(magic_, magic, sizeof magic_);
  tmp_ = file_ + ".XXXXXX";
  const int fd = mkstemp(tmp_.data());
  if (fd < 0) {
    ok_ = false;
    return;
  }
  fchmod(fd, 0644);
  f_ = fdopen(fd, "wb");
  if (!f_) {
    close(fd);
    ok_ = false;
    return;
  }
  static const char zeros[64] = {};
  pos_ = align64(sizeof(Header) + table_.size() * sizeof(uint64_t));
  for (uint64_t left = pos_; left > 0;) {  // header and table, filled in by finish()
    const size_t n = std::min<uint64_t>(left, sizeof zeros);
    ok_ = ok_ && std::fwrite(zeros, 1, n, f_) == n;
    left -= n;
  }
}

FieldWriter::~FieldWriter() {
  if (f_) {
    std::fclose(f_);
    std::remove(tmp_.c_str());
  }
}

bool FieldWriter::begin(size_t field) {
  static const char zeros[64] = {};
  const uint64_t at = align64(pos_);
  ok_ = ok_ && f_ && field < table_.size() / 2 && std::fwrite(zeros, 1, at - pos_, f_) == at - pos_;
  pos_ = at;
  open_ = field;
  table_[2 * field] = pos_;
  table_[2 * field + 1] = 0;
  return ok_;
}

bool FieldWriter::append(const void* data, size_t size) {
  ok_ = ok_ && open_ != SIZE_MAX && std::fwrite(data, 1, size, f_) == size;
  pos_ += size;
  if (open_ != SIZE_MAX) table_[2 * open_ + 1] += size;
  return ok_;
}

void FieldWriter::end() { open_ = SIZE_MAX; }

bool FieldWriter::write(size_t field, const void* data, size_t size) {
  begin(field);
  append(data, size);
  end();
  return ok_;
}

bool FieldWriter::finish(const std::array<uint64_t, 8>& meta) {
  if (!f_) return false;
  Header h{};
  std::memcpy(h.magic, magic_, sizeof h.magic);
  h.count = table_.size() / 2;
  std::memcpy(h.meta, meta.data(), sizeof h.meta);
  ok_ = ok_ && std::fseek(f_, 0, SEEK_SET) == 0 && std::fwrite(&h, sizeof h, 1, f_) == 1 &&
        std::fwrite(table_.data(), sizeof(uint64_t), table_.size(), f_) == table_.size();
  ok_ = std::fclose(f_) == 0 && ok_;
  f_ = nullptr;
  if (!ok_ || std::rename(tmp_.c_str(), file_.c_str()) != 0) {
    std::remove(tmp_.c_str());
    return false;
  }
  return true;
}

Fields hold_fields(const std::array<uint64_t, 8>& meta, std::vector<Blob> fields) {
  Fields r;
  r.meta = meta;
  for (const Blob& b : fields) {
    r.f.emplace_back(b.data, b.size);
    r.bytes += b.size;
  }
  r.backing = std::make_shared<const std::vector<Blob>>(std::move(fields));
  return r;
}

const Overflow* find_key(const Span<Overflow>& s, uint32_t key) {
  const Overflow* it =
      std::lower_bound(s.p, s.p + s.n, key, [](const Overflow& o, uint32_t k) { return o.key < k; });
  return it != s.p + s.n && it->key == key ? it : nullptr;
}

uint64_t hash_bytes(uint64_t seed, std::string_view s) {
  uint64_t h = seed ^ 0xcbf29ce484222325ull;
  for (unsigned char c : s) h = (h ^ c) * 0x100000001b3ull;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  return h ^ (h >> 33);
}

bool is_hex_name(std::string_view s) {
  if (s.size() < 16 || s.size() > 255) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

void pack_hex(std::string_view s, BigVec<char>& out) {
  auto val = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  out.push_back(char(uint8_t(s.size())));
  for (size_t i = 0; i < s.size(); i += 2)
    out.push_back(char((val(s[i]) << 4) | (i + 1 < s.size() ? val(s[i + 1]) : 0)));
}

}  // namespace fplussearch
