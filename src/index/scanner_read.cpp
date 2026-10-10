#include "index/scanner.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <functional>
#include <unordered_map>

#include "index/listing.hpp"
#include "symbols/symbols.hpp"

namespace fplussearch::index_detail {

namespace {

std::string join(const std::string& dir, std::string_view name) {
  std::string s;
  s.reserve(dir.size() + name.size() + 1);
  s = dir;
  if (dir != "/") s += '/';
  s += name;
  return s;
}

}  // namespace

// Builds what copy_dir and read_dir need from the previous index: interned
// names per entry, children per directory, and source file mtimes.
void Scanner::index_old() {
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

uint64_t Scanner::old_code_mtime(uint32_t e) const {
  const Index& o = *old_;
  const uint32_t* it = std::lower_bound(o.code_entry.p, o.code_entry.p + o.code_entry.n, e);
  return it != o.code_entry.p + o.code_entry.n && *it == e ? o.code_mtime[size_t(it - o.code_entry.p)] : kNoMtime;
}

// Copies old entry `c` as an item listed under batch item `parent`.
Item Scanner::copy_item(uint32_t c, uint32_t parent) const {
  const Index& o = *old_;
  const uint64_t seconds = uint64_t(o.mtime[c]) * 1000000000;
  if (o.is_dir(c)) return {0, 0, 0, 0, seconds, kRawDir, false, old_name_[c], parent, o.flags[c]};
  const uint64_t code = old_code_mtime(c);
  return {0, 0, 0, o.size(c), code != kNoMtime ? code : seconds, o.file_kind[c - o.dirs], code != kNoMtime,
          old_name_[c], parent, o.flags[c]};
}

// Lists an unchanged directory with changes below it from the previous index.
void Scanner::copy_dir(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs) {
  items.clear();
  subdirs.clear();
  for (uint32_t i = old_child_start_[w.old]; i < old_child_start_[w.old + 1]; ++i) {
    const uint32_t c = old_children_[i];
    items.push_back(copy_item(c, kNoOld));
    if (old_->is_dir(c)) subdirs.push_back({uint32_t(items.size() - 1), join(w.path, old_->name(c)), c});
  }
}

// Copies a subtree without changes from the previous index in one batch.
void Scanner::copy_tree(const Work& w, std::vector<Item>& items, std::vector<Subdir>& subdirs) {
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

void Scanner::read_dir(const Work& w, std::vector<char>& buf, std::vector<char>& names, std::vector<Item>& items,
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

}  // namespace fplussearch::index_detail
