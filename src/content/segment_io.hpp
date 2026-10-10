#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "content/segment.hpp"
#include "storage/field_writer.hpp"
#include "storage/mapped_array.hpp"

namespace fplussearch::content_detail {

constexpr char kMagic[8] = {'F', 'P', 'S', 'C', 'O', 'N', 'T', '2'};
enum Field : size_t { kKey, kOff, kPost, kPathOff, kPaths, kSize, kMtime, kByPath, kRank, kFields };

struct DocMeta {
  std::string_view path;
  uint64_t size;
  uint32_t mtime;
  int8_t rank;
};

std::string seg_file(const std::string& dir, uint64_t id, const char* ext);
std::shared_ptr<ContentIndex::Segment> map_segment(const std::string& dir, uint64_t id);
std::vector<uint64_t> load_dead(const std::string& dir, uint64_t id, uint32_t ndocs);
void save_dead(const std::string& dir, uint64_t id, const std::vector<uint64_t>& dead);
size_t live_count(const std::vector<uint64_t>& dead, uint32_t ndocs);

// Encodes postings (a bitset when dense, else delta varints each followed
// by the doc's follow mask) and writes a segment. `next` fills one
// trigram's ascending doc ids and their masks, and the trigram, ascending,
// until it returns false.
template <typename Next>
// Writes a segment, streaming the postings to the file as `next` produces
// them (they are the bulk of it), so a big segment never sits in memory.
bool write_segment(const std::string& dir, uint64_t id, const std::vector<DocMeta>& docs, Next&& next,
                   std::span<const BloomView> blooms = {}) {
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
  if (!w.finish(meta)) return false;
  if (!blooms.empty() && blooms.size() == docs.size()) {
    const std::string file = seg_file(dir, id, "blm"), tmp = file + ".tmp";
    const int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
      const uint32_t hdr[2] = {0x334d4c42u, uint32_t(blooms.size())};
      bool ok = write(fd, hdr, sizeof hdr) == ssize_t(sizeof hdr);
      for (const auto& r : blooms) {
        const uint32_t sw = uint32_t(r.short_bits.size()), lw = uint32_t(r.long_bits.size());
        ok = ok && write(fd, &sw, 4) == 4 && write(fd, &lw, 4) == 4 && write(fd, &r.mtime_ns, 8) == 8 && write(fd, &r.ctime_ns, 8) == 8 &&
             write(fd, r.short_bits.data(), size_t(sw) * 8) == ssize_t(size_t(sw) * 8) &&
             write(fd, r.long_bits.data(), size_t(lw) * 8) == ssize_t(size_t(lw) * 8);
      }
      close(fd);
      if (!ok || rename(tmp.c_str(), file.c_str()) != 0) unlink(tmp.c_str());
    }
  }
  return true;
}

}  // namespace fplussearch::content_detail
