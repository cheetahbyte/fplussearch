#include "storage/fields.hpp"
#include "storage/format.hpp"
#include "storage/section.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>

namespace fplussearch {

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
  // the daemon refreshes) must never write into a file another one publishes.
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

}  // namespace fplussearch
