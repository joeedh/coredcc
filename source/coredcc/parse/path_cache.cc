#include "coredcc/parse/path_cache.h"

#include "litestl/util/alloc.h"

namespace coredcc {

void PathPtr::retain()
{
  if (n_) {
    n_->refs.fetch_add(1, std::memory_order_relaxed);
  }
}

void PathPtr::release()
{
  if (n_ && n_->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    litestl::alloc::Delete(n_);
  }
  n_ = nullptr;
}

PathCache::PathCache(Runtime *) {}

PathCache::~PathCache()
{
  clear();
}

void PathCache::unlink(detail::PathNode *n)
{
  (n->newer ? n->newer->older : newest_) = n->older;
  (n->older ? n->older->newer : oldest_) = n->newer;
  n->newer = n->older = nullptr;
}

void PathCache::pushNewest(detail::PathNode *n)
{
  n->older = newest_;
  n->newer = nullptr;
  (newest_ ? newest_->newer : oldest_) = n;
  newest_ = n;
}

Result<PathPtr> PathCache::get(std::string_view text, size_t *errorOffset)
{
  const uint64_t h = detail::fnv1a64(text);
  if (detail::PathNode *const *found = byHash_.lookup_ptr(h)) {
    detail::PathNode *n = *found;
    if (n->text == text) {
      hits_++;
      unlink(n);
      pushNewest(n);
      PathPtr p(n);
      p.retain();
      return p;
    }
  }
  misses_++;
  Result<Path> parsed = parsePath(text, errorOffset);
  if (!parsed.ok()) {
    return parsed.status();
  }
  detail::PathNode *n = litestl::alloc::New<detail::PathNode>("coredcc::PathNode");
  n->text = std::string(text);
  n->path = std::move(parsed).value();
  PathPtr result(n);
  if (!byHash_.lookup_ptr(h)) {
    while (size_ >= capacity_) {
      evictOne();
    }
    result.retain(); /* the cache's reference */
    byHash_.add(h, n);
    pushNewest(n);
    size_++;
  }
  return result;
}

void PathCache::evictOne()
{
  detail::PathNode *n = oldest_;
  unlink(n);
  byHash_.remove(detail::fnv1a64(n->text));
  size_--;
  PathPtr drop(n); /* releases the cache's reference */
}

void PathCache::setCapacity(int capacity)
{
  capacity_ = capacity < 1 ? 1 : capacity;
  while (size_ > capacity_) {
    evictOne();
  }
}

void PathCache::clear()
{
  while (size_ > 0) {
    evictOne();
  }
}

Status PathCache::validate() const
{
  int count = 0;
  const detail::PathNode *prev = nullptr;
  for (const detail::PathNode *n = newest_; n; n = n->older) {
    detail::PathNode *const *found = byHash_.lookup_ptr(detail::fnv1a64(n->text));
    if (n->newer != prev || !found || *found != n || n->refs.load() < 1) {
      return Status::error(StatusCode::Internal, "PathCache: the LRU list is broken");
    }
    prev = n;
    count++;
  }
  if (prev != oldest_ || count != size_ || int(byHash_.size()) != size_ || size_ > capacity_) {
    return Status::error(StatusCode::Internal, "PathCache: size, list and index disagree");
  }
  return Status();
}

} // namespace coredcc
