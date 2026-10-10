#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <pthread.h>
#include <sys/qos.h>
#include <mutex>
#include <thread>
#include <vector>

namespace fplussearch {

// Persistent workers; the caller thread also takes jobs. Jobs are claimed from
// an atomic counter, so threads that wake late simply get fewer of them.
class Pool {
 public:
  explicit Pool(unsigned threads) {
    // Interactive QoS keeps searches on performance cores at full clock.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (unsigned i = 1; i < threads; ++i)
      workers_.emplace_back([this] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        loop();
      });
  }

  ~Pool() {
    {
      std::lock_guard lk(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
  }

  unsigned size() const { return unsigned(workers_.size()) + 1; }

  void run(size_t jobs, const std::function<void(size_t)>& fn) {
    auto b = std::make_shared<Batch>();
    b->fn = &fn;
    b->n = jobs;
    {
      std::lock_guard lk(m_);
      batch_ = b;
      gen_.fetch_add(1, std::memory_order_release);
    }
    cv_.notify_all();
    work(*b);
    while (b->done.load(std::memory_order_acquire) < jobs) std::this_thread::yield();
  }

 private:
  struct Batch {
    const std::function<void(size_t)>* fn = nullptr;
    size_t n = 0;
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
  };

  static void work(Batch& b) {
    for (size_t i; (i = b.next.fetch_add(1, std::memory_order_relaxed)) < b.n;) {
      (*b.fn)(i);
      b.done.fetch_add(1, std::memory_order_release);
    }
  }

  void loop() {
    uint64_t seen = 0;
    for (;;) {
      // Stay hot for a while after each batch: keystrokes arrive ~100 ms apart
      // and an idle P-core cluster takes several ms to ramp back up, which is
      // ~10x the cost of a search. Workers sleep once typing pauses.
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
      while (gen_.load(std::memory_order_acquire) == seen && !stop_.load(std::memory_order_relaxed) &&
             std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
      std::shared_ptr<Batch> b;
      {
        std::unique_lock lk(m_);
        cv_.wait(lk, [&] { return stop_ || gen_.load(std::memory_order_relaxed) != seen; });
        if (stop_) return;
        seen = gen_.load(std::memory_order_relaxed);
        b = batch_;
      }
      work(*b);
    }
  }

  std::vector<std::thread> workers_;
  std::mutex m_;
  std::condition_variable cv_;
  std::shared_ptr<Batch> batch_;
  std::atomic<uint64_t> gen_{0};
  std::atomic<bool> stop_{false};
};

}  // namespace fplussearch
