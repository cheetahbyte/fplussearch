#include "index/scanner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <random>
#include <thread>

#include "storage/fields.hpp"
#include "storage/section.hpp"

namespace fplussearch::index_detail {

namespace {

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

}  // namespace

// Sorts name ids by name: bucketed by first byte, buckets sorted in parallel.
void Scanner::sort_names(BigVec<uint32_t>& ids) const {
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
void Scanner::lay_out_dirs(Built& b, const BigVec<uint32_t>& new_id, uint32_t dirs) {
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
Built Scanner::finish() {
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

}  // namespace fplussearch::index_detail
