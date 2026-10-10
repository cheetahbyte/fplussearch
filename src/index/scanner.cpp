#include "index/scanner.hpp"

#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace fplussearch::index_detail {

Scanner::Scanner(std::string root, ScanProgress* progress, const Index* old, const Changes* changes)
    : root_(std::move(root)), progress_(progress), excluded_(excluded_dirs(root_)), old_(old) {
  if (changes) {
    dirty_.insert(changes->dirs.begin(), changes->dirs.end());
    trees_.insert(changes->trees.begin(), changes->trees.end());
    for (const auto* set : {&changes->dirs, &changes->trees})
      for (const std::string& p : *set)
        for (size_t i = p.find('/', 1); i != std::string::npos; i = p.find('/', i + 1)) above_.insert(p.substr(0, i));
    above_.insert(root_);
  }
}

Built Scanner::run(unsigned threads, bool background) {
  add_entry(names_.intern("", 0), 0, 0, kRawDir, 0, 0);
  if (old_) index_old();
  for (unsigned i = 0; i < threads; ++i) queues_.push_back(std::make_unique<Queue>());
  queues_[0]->q.push_back({0, root_, old_ ? 0 : kNoOld});
  pending_ = 1;

  std::vector<std::thread> pool;
  for (unsigned i = 0; i < threads; ++i)
    pool.emplace_back([this, background, i] {
      // In the foreground someone waits for the index: keep the scan on
      // the performance cores.
      pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
      // Listing a folder of dataless (cloud placeholder) files must not
      // download them or wait on their file provider.
      setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
      worker(i);
    });
  for (auto& t : pool) t.join();
  return finish();
}

void Scanner::add_entry(uint32_t name, uint32_t parent, uint64_t size, uint8_t kind, uint64_t mtime_ns, uint8_t flags) {
  if (kind != kRawDir && size >= UINT32_MAX) big_.push_back({uint32_t(name_.size()), 0, size});
  name_.push_back(name);
  parent_.push_back(parent);
  size_.push_back(uint32_t(std::min<uint64_t>(size, UINT32_MAX)));
  kind_.push_back(kind);
  mtime_.push_back(uint32_t(std::min<uint64_t>(mtime_ns / 1000000000, UINT32_MAX)));
  flags_.push_back(flags);
}

// Takes the newest directory from this thread's queue, or steals the
// oldest from another's: threads stay in separate subtrees instead of
// contending on the same parent directory in the kernel.
bool Scanner::take(unsigned self, Work& w) {
  const size_t n = queues_.size();
  for (size_t k = 0; k < n; ++k) {
    Queue& qu = *queues_[(self + k) % n];
    std::lock_guard lk(qu.m);
    if (qu.q.empty()) continue;
    if (k == 0) {
      w = std::move(qu.q.back());
      qu.q.pop_back();
    } else {
      w = std::move(qu.q.front());
      qu.q.pop_front();
    }
    return true;
  }
  return false;
}

void Scanner::worker(unsigned self) {
  std::vector<char> buf(256 * 1024);
  std::vector<char> names;
  std::vector<Item> items;
  std::vector<Subdir> subdirs;
  for (unsigned idle = 0;;) {
    Work w;
    if (!take(self, w)) {
      if (pending_.load(std::memory_order_acquire) == 0) return;
      if (++idle < 64) std::this_thread::yield();
      else std::this_thread::sleep_for(std::chrono::microseconds(100));
      continue;
    }
    idle = 0;
    if (w.old != kNoOld && !dirty_.contains(w.path) && !trees_.contains(w.path)) {
      if (above_.contains(w.path)) copy_dir(w, items, subdirs);
      else copy_tree(w, items, subdirs);
    } else {
      read_dir(w, buf, names, items, subdirs);
    }
    publish(self, w, names, items, subdirs);
  }
}

void Scanner::publish(unsigned self, const Work& w, const std::vector<char>& names, const std::vector<Item>& items,
                      std::vector<Subdir>& subdirs) {
  uint32_t base;
  {
    std::lock_guard lk(m_);
    base = uint32_t(name_.size());
    if (base + items.size() >= UINT32_MAX) {
      subdirs.clear();
    } else {
      for (const auto& it : items) {
        if (it.code) code_.push_back({uint32_t(name_.size()), it.mtime});
        const uint32_t name = it.name != kNoOld ? it.name : names_.intern({names.data() + it.off, it.len}, it.hash);
        add_entry(name, it.parent == kNoOld ? w.id : base + it.parent, it.size, it.kind, it.mtime, it.flags);
      }
      if (progress_) progress_->entries.store(name_.size(), std::memory_order_relaxed);
    }
  }
  if (!subdirs.empty()) {
    pending_.fetch_add(subdirs.size(), std::memory_order_relaxed);
    Queue& qu = *queues_[self];
    std::lock_guard lk(qu.m);
    for (auto& sd : subdirs) qu.q.push_back({base + sd.item, std::move(sd.path), sd.old});
  }
  pending_.fetch_sub(1, std::memory_order_release);
}

}  // namespace fplussearch::index_detail
