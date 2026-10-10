#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "index/index.hpp"

namespace fplussearch {

// The newest FSEvents id on this machine.
uint64_t current_event_id();

// Follows FSEvents under `root` from event `since`: first the history since
// then, after that live changes. Events are per directory.
class Watcher {
 public:
  // `latency`: how long FSEvents may hold events back to batch them.
  Watcher(std::string root, uint64_t since, std::vector<std::string> ignore, double latency = 0.1);
  ~Watcher();
  Watcher(const Watcher&) = delete;
  Watcher& operator=(const Watcher&) = delete;

  enum class Ready { No, Changes, Rescan };

  // Waits up to `timeout`. Changes are ready once the history is replayed
  // and no event arrived for `quiet`, or the oldest has waited `max_wait`.
  // Rescan means FSEvents lost track and the whole root must be scanned.
  Ready wait(std::chrono::milliseconds timeout, std::chrono::milliseconds quiet, std::chrono::milliseconds max_wait);

  // Takes the pending changes.
  Changes take();

  void on_events(size_t n, char** paths, const uint32_t* flags, const uint64_t* ids);

 private:
  bool wanted(const std::string& path) const;

  std::string root_;
  std::vector<std::string> ignore_;
  void* stream_ = nullptr;
  void* queue_ = nullptr;

  std::mutex m_;
  std::condition_variable cv_;
  std::set<std::string> dirs_, trees_;
  uint64_t last_id_;
  bool replayed_ = false;
  bool flush_ = false;  // apply pending changes without waiting
  bool rescan_ = false;
  std::chrono::steady_clock::time_point first_, last_;
};

}  // namespace fplussearch
