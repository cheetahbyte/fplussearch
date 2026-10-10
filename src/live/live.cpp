#include "live/live.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdlib>

namespace fplussearch {

Live::Live(Options o, Engine& engine) : opt_(std::move(o)), engine_(engine) {
  ignore_ = excluded_dirs(opt_.root);
  ignore_.push_back(opt_.cache_file.substr(0, opt_.cache_file.rfind('/')));  // our own writes
  if (opt_.content) {
    std::string area = opt_.content_area;
    if (area.empty()) area = opt_.root != "/" ? opt_.root : std::getenv("HOME") ? std::getenv("HOME") : "";
    if (char* real = realpath(area.c_str(), nullptr)) {
      area = real;
      std::free(real);
    }
    const std::string dir = opt_.cache_file.substr(0, opt_.cache_file.rfind(".bin")) + ".content";
    if (!area.empty()) content_ = std::make_unique<ContentIndex>(dir, area);
  }
}

Live::~Live() {
  stop();
  if (lock_fd_ >= 0) close(lock_fd_);
}

void Live::start(std::shared_ptr<const Index> ix, std::function<void()> on_change) {
  lock_fd_ = open((opt_.cache_file + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  owner_ = lock_fd_ >= 0 && flock(lock_fd_, LOCK_EX | LOCK_NB) == 0;
  on_change_ = std::move(on_change);
  if (content_) {
    content_->open();
    if (owner_) content_thread_ = std::thread([this] { content_loop(); });
  }
  if (ix) publish(fresh_state(ix));
  thread_ = std::thread([this, ix] { run(ix); });
}

void Live::content_sync(std::shared_ptr<const Index> ix) {
  if (!content_ || !owner_) return;
  std::lock_guard lk(content_m_);
  content_ix_ = std::move(ix);
  content_cv_.notify_one();
}

void Live::content_update(const Changes& c) {
  if (!content_ || !owner_) return;
  std::lock_guard lk(content_m_);
  content_dirs_.insert(content_dirs_.end(), c.dirs.begin(), c.dirs.end());
  content_trees_.insert(content_trees_.end(), c.trees.begin(), c.trees.end());
  content_cv_.notify_one();
}

void Live::content_loop() {
  for (;;) {
    std::shared_ptr<const Index> ix;
    std::vector<std::string> dirs, trees;
    {
      std::unique_lock lk(content_m_);
      content_cv_.wait(lk, [&] { return stop_ || content_ix_ || !content_dirs_.empty() || !content_trees_.empty(); });
      if (stop_) return;
      ix = std::move(content_ix_);
      dirs.swap(content_dirs_);
      trees.swap(content_trees_);
    }
    content_busy_ = true;
    if (ix) content_->sync(*ix, true);
    if (!dirs.empty() || !trees.empty()) content_->update_paths(dirs, trees);
    content_busy_ = false;
  }
}

void Live::stop() {
  stop_ = true;
  if (thread_.joinable()) {
    if (scanning_) thread_.detach();
    else thread_.join();
  }
  // A compaction finishing writes a cache file the next start can use; one
  // cut short only leaves a temporary file behind.
  if (compactor_.joinable()) compactor_.detach();
  if (content_thread_.joinable()) {
    {
      std::lock_guard lk(content_m_);
      content_cv_.notify_all();
    }
    if (content_busy_) content_thread_.detach();  // a sync can take a while; its segments are written atomically
    else content_thread_.join();
  }
}

}  // namespace fplussearch
