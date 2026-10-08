#include "live.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <algorithm>
#include <string_view>

namespace fplussearch {

namespace {

bool under(std::string_view path, std::string_view dir) {
  return dir == "/" || path == dir || (path.starts_with(dir) && path[dir.size()] == '/');
}

// Changes on the Data volume may be reported through its own mount point
// rather than the firmlinked path the scan sees.
std::string normalize(std::string_view p) {
  constexpr std::string_view kData = "/System/Volumes/Data";
  if (p.starts_with(kData) && (p.size() == kData.size() || p[kData.size()] == '/')) p.remove_prefix(kData.size());
  while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
  return p.empty() ? "/" : std::string(p);
}

void callback(ConstFSEventStreamRef, void* info, size_t n, void* paths, const FSEventStreamEventFlags flags[],
              const FSEventStreamEventId ids[]) {
  static_cast<Watcher*>(info)->on_events(n, static_cast<char**>(paths), flags, ids);
}

}  // namespace

uint64_t current_event_id() { return FSEventsGetCurrentEventId(); }

Watcher::Watcher(std::string root, uint64_t since, std::vector<std::string> ignore, double latency)
    : root_(std::move(root)), ignore_(std::move(ignore)), last_id_(since) {
  CFStringRef path = CFStringCreateWithCString(nullptr, root_.c_str(), kCFStringEncodingUTF8);
  CFArrayRef paths = CFArrayCreate(nullptr, reinterpret_cast<const void**>(&path), 1, &kCFTypeArrayCallBacks);
  FSEventStreamContext ctx{0, this, nullptr, nullptr, nullptr};
  FSEventStreamRef s = FSEventStreamCreate(nullptr, callback, &ctx, paths, since, latency, kFSEventStreamCreateFlagNone);
  CFRelease(paths);
  CFRelease(path);
  dispatch_queue_t q = dispatch_queue_create("fplussearch.fsevents", DISPATCH_QUEUE_SERIAL);
  FSEventStreamSetDispatchQueue(s, q);
  if (!FSEventStreamStart(s)) rescan_ = true;
  stream_ = s;
  queue_ = q;
}

Watcher::~Watcher() {
  auto s = static_cast<FSEventStreamRef>(stream_);
  auto q = static_cast<dispatch_queue_t>(queue_);
  FSEventStreamStop(s);
  FSEventStreamInvalidate(s);
  dispatch_sync_f(q, nullptr, [](void*) {});  // let a running callback finish
  FSEventStreamRelease(s);
  dispatch_release(q);
}

bool Watcher::wanted(const std::string& path) const {
  if (!under(path, root_)) return false;
  return std::none_of(ignore_.begin(), ignore_.end(), [&](const std::string& d) { return under(path, d); });
}

void Watcher::on_events(size_t n, char** paths, const uint32_t* flags, const uint64_t* ids) {
  std::lock_guard lk(m_);
  const auto now = std::chrono::steady_clock::now();
  const bool was_pending = !dirs_.empty() || !trees_.empty();
  for (size_t i = 0; i < n; ++i) {
    const uint32_t f = flags[i];
    last_id_ = std::max(last_id_, ids[i]);
    if (f & kFSEventStreamEventFlagHistoryDone) {
      replayed_ = flush_ = true;  // apply the replayed history right away
      continue;
    }
    if (f & (kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagEventIdsWrapped)) {
      rescan_ = true;
      continue;
    }
    const std::string p = normalize(paths[i]);
    const bool lost = f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped |
                           kFSEventStreamEventFlagKernelDropped);
    if (lost && under(root_, p)) {
      rescan_ = true;  // history lost for the root or above it
      continue;
    }
    if (!wanted(p)) continue;
    (lost ? trees_ : dirs_).insert(p);
  }
  if (!was_pending && (!dirs_.empty() || !trees_.empty())) first_ = now;
  last_ = now;
  cv_.notify_all();
}

Watcher::Ready Watcher::wait(std::chrono::milliseconds timeout, std::chrono::milliseconds quiet,
                             std::chrono::milliseconds max_wait) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::unique_lock lk(m_);
  for (;;) {
    if (rescan_) return Ready::Rescan;
    const auto now = std::chrono::steady_clock::now();
    auto at = deadline;
    if (replayed_ && (!dirs_.empty() || !trees_.empty())) {
      const auto ready = flush_ ? now : std::min(last_ + quiet, first_ + max_wait);
      if (now >= ready) return Ready::Changes;
      at = std::min(at, ready);
    }
    if (now >= deadline) return Ready::No;
    cv_.wait_until(lk, at);
  }
}

Changes Watcher::take() {
  std::lock_guard lk(m_);
  Changes c;
  c.dirs.assign(dirs_.begin(), dirs_.end());
  c.trees.assign(trees_.begin(), trees_.end());
  c.event_id = last_id_;
  dirs_.clear();
  trees_.clear();
  flush_ = false;
  return c;
}

}  // namespace fplussearch
