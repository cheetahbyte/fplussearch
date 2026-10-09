#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace fplussearch {

class DeadWords {
  static constexpr size_t kWords = 512;
  using Page = std::array<uint64_t, kWords>;
  std::vector<std::shared_ptr<Page>> pages_;
 public:
  void assign(size_t n, uint64_t value) {
    pages_.clear();
    for (size_t i = 0; i < n; i += kWords) {
      auto page = std::make_shared<Page>();
      page->fill(value);
      pages_.push_back(std::move(page));
    }
  }
  uint64_t operator[](size_t i) const { return (*pages_[i / kWords])[i % kWords]; }
  uint64_t& operator[](size_t i) {
    auto& page = pages_[i / kWords];
    if (page.use_count() != 1) page = std::make_shared<Page>(*page);
    return (*page)[i % kWords];
  }
};

class DeadView {
  const DeadWords* pages_ = nullptr;
  const std::vector<uint64_t>* flat_ = nullptr;
 public:
  DeadView() = default;
  DeadView(std::nullptr_t) {}
  DeadView(const DeadWords* pages) : pages_(pages) {}
  DeadView(const std::vector<uint64_t>* flat) : flat_(flat) {}
  explicit operator bool() const { return pages_ || flat_; }
  const DeadView& operator*() const { return *this; }
  uint64_t operator[](size_t i) const { return pages_ ? (*pages_)[i] : (*flat_)[i]; }
};

template <typename Value>
class OverlayMap {
  static constexpr size_t kBlock = 256;
  using Block = std::map<std::string, Value>;
  std::vector<std::shared_ptr<Block>> blocks_;
  size_t size_ = 0;
  size_t block_for(const std::string& key) const {
    if (blocks_.size() == 1) return 0;
    auto it = std::upper_bound(blocks_.begin(), blocks_.end(), key,
        [](const std::string& k, const auto& b) { return k < b->begin()->first; });
    return it == blocks_.begin() ? 0 : size_t(it - blocks_.begin() - 1);
  }
  Block& writable(size_t i) {
    if (blocks_[i].use_count() != 1) blocks_[i] = std::make_shared<Block>(*blocks_[i]);
    return *blocks_[i];
  }
 public:
  class iterator {
    const OverlayMap* owner_ = nullptr;
    size_t block_ = 0;
    typename Block::const_iterator it_;
    friend class OverlayMap;
    iterator(const OverlayMap* owner, size_t block, typename Block::const_iterator it = {})
        : owner_(owner), block_(block), it_(it) {}
   public:
    const auto& operator*() const { return *it_; }
    const auto* operator->() const { return &*it_; }
    iterator& operator++() {
      if (++it_ == owner_->blocks_[block_]->end()) {
        if (++block_ < owner_->blocks_.size()) it_ = owner_->blocks_[block_]->begin();
      }
      return *this;
    }
    bool operator==(const iterator& other) const {
      return owner_ == other.owner_ && block_ == other.block_ &&
          (block_ == owner_->blocks_.size() || it_ == other.it_);
    }
  };
  size_t size() const { return size_; }
  iterator end() const { return iterator(this, blocks_.size()); }
  iterator begin() const { return blocks_.empty() ? end() : iterator(this, 0, blocks_[0]->begin()); }
  iterator lower_bound(const std::string& key) const {
    if (blocks_.empty()) return end();
    size_t i = block_for(key);
    auto it = blocks_[i]->lower_bound(key);
    if (it != blocks_[i]->end()) return iterator(this, i, it);
    return ++i == blocks_.size() ? end() : iterator(this, i, blocks_[i]->begin());
  }
  iterator find(const std::string& key) const {
    auto it = lower_bound(key);
    return it != end() && it->first == key ? it : end();
  }
  void set(const std::string& key, Value value) {
    if (blocks_.empty()) blocks_.push_back(std::make_shared<Block>());
    const size_t i = block_for(key);
    auto& block = writable(i);
    const auto [it, added] = block.insert_or_assign(key, std::move(value));
    (void)it;
    size_ += added;
    if (block.size() > kBlock * 2) {
      auto right = std::make_shared<Block>();
      auto mid = block.begin();
      std::advance(mid, block.size() / 2);
      right->insert(mid, block.end());
      block.erase(mid, block.end());
      blocks_.insert(blocks_.begin() + i + 1, std::move(right));
    }
  }
  void erase(const std::string& key) {
    if (blocks_.empty()) return;
    size_t i = block_for(key);
    if (!blocks_[i]->contains(key)) return;
    size_ -= writable(i).erase(key);
    if (blocks_[i]->empty()) blocks_.erase(blocks_.begin() + i);
  }
  void erase_range(const std::string& lo, const std::string& hi) {
    if (blocks_.empty()) return;
    size_t i = block_for(lo);
    while (i < blocks_.size() && blocks_[i]->begin()->first < hi) {
      const auto& block = *blocks_[i];
      if (block.begin()->first >= lo && block.rbegin()->first < hi) {
        size_ -= block.size();
        blocks_.erase(blocks_.begin() + i);
      } else {
        auto first = block.lower_bound(lo), last = block.lower_bound(hi);
        if (first != last) {
          size_ -= std::distance(first, last);
          auto& changed = writable(i);
          changed.erase(changed.lower_bound(lo), changed.lower_bound(hi));
        }
        ++i;
      }
    }
  }
};

}  // namespace fplussearch
