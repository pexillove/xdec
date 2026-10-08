// A copy-on-write list of sinks: registration takes a lock and publishes a
// new vector; delivery takes the lock only long enough to copy one
// shared_ptr, so a slow sink never holds up registration or other emitters.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "xdec/obs/log.h"

namespace xdec::obs::detail {

template <class Sink>
class SinkList {
 public:
  struct Entry {
    SinkId id;
    std::shared_ptr<Sink> sink;
  };
  using Snapshot = std::shared_ptr<const std::vector<Entry>>;

  SinkId add(std::shared_ptr<Sink> sink) {
    const std::lock_guard lock{mutex_};
    auto next = std::make_shared<std::vector<Entry>>(*current_);
    const SinkId id = nextId_++;
    next->push_back(Entry{id, std::move(sink)});
    current_ = std::move(next);
    size_.store(current_->size(), std::memory_order_relaxed);
    return id;
  }

  void remove(SinkId id) {
    const std::lock_guard lock{mutex_};
    auto next = std::make_shared<std::vector<Entry>>(*current_);
    std::erase_if(*next, [id](const Entry& entry) { return entry.id == id; });
    current_ = std::move(next);
    size_.store(current_->size(), std::memory_order_relaxed);
  }

  void clear() {
    const std::lock_guard lock{mutex_};
    current_ = std::make_shared<std::vector<Entry>>();
    size_.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] Snapshot snapshot() const {
    const std::lock_guard lock{mutex_};
    return current_;
  }

  [[nodiscard]] bool empty() const noexcept { return size_.load(std::memory_order_relaxed) == 0; }

 private:
  mutable std::mutex mutex_;
  Snapshot current_ = std::make_shared<std::vector<Entry>>();
  SinkId nextId_ = 1;
  std::atomic<std::size_t> size_{0};
};

}  // namespace xdec::obs::detail
