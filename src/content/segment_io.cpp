#include "content/segment_io.hpp"

#include <sys/mman.h>
#include <sys/stat.h>

#include <cstring>

#include "storage/fields.hpp"

namespace fplussearch::content_detail {

std::string seg_file(const std::string& dir, uint64_t id, const char* ext) {
  char name[64];
  std::snprintf(name, sizeof name, "/seg-%06llu.%s", static_cast<unsigned long long>(id), ext);
  return dir + name;
}

// The container format of storage/fields.cpp, mapped without its read-ahead hint:
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
  if (!ok) return nullptr;
  seg->path_order.resize(seg->ndocs);
  for (uint32_t i = 0; i < seg->ndocs; ++i) {
    const uint32_t d = seg->by_path[i];
    if (d >= seg->ndocs) return nullptr;
    seg->path_order[d] = i;
  }
  const int bf = open(seg_file(dir, id, "blm").c_str(), O_RDONLY | O_CLOEXEC);
  if (bf >= 0) {
    struct stat bst{};
    if (fstat(bf, &bst) == 0 && bst.st_size >= 8 + int64_t(seg->ndocs) * 24) {
      const size_t length = size_t(bst.st_size);
      void* mapped = mmap(nullptr, length, PROT_READ, MAP_SHARED, bf, 0);
      if (mapped != MAP_FAILED) {
        auto backing = std::shared_ptr<const void>(mapped, [length](const void* p) { munmap(const_cast<void*>(p), length); });
        const char* data = static_cast<const char*>(mapped);
        uint32_t magic, n;
        std::memcpy(&magic, data, 4);
        std::memcpy(&n, data + 4, 4);
        if (magic == 0x334d4c42u && n == seg->ndocs) {
          auto blooms = std::make_shared<std::vector<BloomView>>(n);
          size_t at = 8;
          bool valid = true;
          for (auto& r : *blooms) {
            if (length - at < 24) { valid = false; break; }
            uint32_t sw, lw;
            std::memcpy(&sw, data + at, 4);
            std::memcpy(&lw, data + at + 4, 4);
            std::memcpy(&r.mtime_ns, data + at + 8, 8);
            std::memcpy(&r.ctime_ns, data + at + 16, 8);
            at += 24;
            constexpr size_t max_words = (kMaxContentFile * 3 + 63) / 64;
            if (sw > max_words || lw > max_words || uint64_t(sw) * 8 + uint64_t(lw) * 8 > length - at) {
              valid = false;
              break;
            }
            r.short_bits = {reinterpret_cast<const uint64_t*>(data + at), sw};
            at += size_t(sw) * 8;
            r.long_bits = {reinterpret_cast<const uint64_t*>(data + at), lw};
            at += size_t(lw) * 8;
          }
          if (valid && at == length) {
            seg->blooms = std::move(blooms);
            seg->bloom_backing = std::move(backing);
            seg->bytes += length;
          }
        }
      }
    }
    close(bf);
  }
  return seg;
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

}  // namespace fplussearch::content_detail
