#pragma once

#include "coredcc/base/runtime.h"
#include "coredcc/parse/path.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <atomic>
#include <string>
#include <string_view>
#include <utility>

namespace coredcc {

namespace detail {
struct PathNode {
  std::atomic<int> refs{1};
  std::string text;
  Path path;
  uint64_t lastUse = 0;
};
} // namespace detail

/**
 * A shared, immutable parsed path. Copies share one node; the node lives while any
 * PathPtr (or the cache) holds it, so a PathPtr stays valid after eviction. The count
 * is atomic: PathPtrs may cross threads, the cache itself may not.
 */
class PathPtr {
 public:
  PathPtr() = default;
  PathPtr(const PathPtr &b) : n_(b.n_)
  {
    retain();
  }
  PathPtr(PathPtr &&b) noexcept : n_(std::exchange(b.n_, nullptr)) {}
  PathPtr &operator=(const PathPtr &b)
  {
    PathPtr tmp(b);
    std::swap(n_, tmp.n_);
    return *this;
  }
  PathPtr &operator=(PathPtr &&b) noexcept
  {
    PathPtr tmp(std::move(b));
    std::swap(n_, tmp.n_);
    return *this;
  }
  ~PathPtr()
  {
    release();
  }

  explicit operator bool() const
  {
    return n_ != nullptr;
  }
  const Path &operator*() const
  {
    return n_->path;
  }
  const Path *operator->() const
  {
    return &n_->path;
  }
  /** The text it was parsed from. */
  const std::string &text() const
  {
    return n_->text;
  }
  /** Number of holders, including the cache. For tests. */
  int useCount() const
  {
    return n_ ? n_->refs.load() : 0;
  }

 private:
  friend class PathCache;
  /** Adopts one reference. */
  explicit PathPtr(detail::PathNode *n) : n_(n) {}
  void retain();
  void release();

  detail::PathNode *n_ = nullptr;
};

/**
 * Parsed paths by text, bounded with LRU eviction. A Runtime extension
 * (`rt.ext<PathCache>()`). Not thread-safe. Failed parses are not cached.
 */
class PathCache {
 public:
  static constexpr const char *kExtName = "coredcc.PathCache";
  static constexpr int kDefaultCapacity = 1024;

  explicit PathCache(Runtime *rt);
  ~PathCache();
  PathCache(const PathCache &) = delete;
  PathCache &operator=(const PathCache &) = delete;

  /** The parsed path for `text`, from the cache or parsed now (see parsePath). */
  Result<PathPtr> get(std::string_view text, size_t *errorOffset = nullptr);

  int size() const
  {
    return int(nodes_.size());
  }
  int capacity() const
  {
    return capacity_;
  }
  /** At least 1; evicts down to the new capacity. */
  void setCapacity(int capacity);
  void clear();
  uint64_t hits() const
  {
    return hits_;
  }
  uint64_t misses() const
  {
    return misses_;
  }

  Status validate() const;

 private:
  void evictOne();

  int capacity_ = kDefaultCapacity;
  uint64_t tick_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  /* By text hash. A hash collision between two texts just leaves the second uncached. */
  litestl::util::Map<uint64_t, detail::PathNode *> byHash_;
  litestl::util::Vector<detail::PathNode *> nodes_;
};

} // namespace coredcc
