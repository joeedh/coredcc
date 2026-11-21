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

Result<PathPtr> PathCache::get(std::string_view text, size_t *errorOffset)
{
  const uint64_t h = detail::fnv1a64(text);
  if (detail::PathNode *const *found = byHash_.lookup_ptr(h)) {
    detail::PathNode *n = *found;
    if (n->text == text) {
      hits_++;
      n->lastUse = ++tick_;
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
  n->lastUse = ++tick_;
  PathPtr result(n);
  if (!byHash_.lookup_ptr(h)) {
    while (size() >= capacity_) {
      evictOne();
    }
    result.retain(); /* the cache's reference */
    byHash_.add(h, n);
    nodes_.append(n);
  }
  return result;
}

void PathCache::evictOne()
{
  int oldest = 0;
  for (int i = 1; i < size(); i++) {
    if (nodes_[i]->lastUse < nodes_[oldest]->lastUse) {
      oldest = i;
    }
  }
  detail::PathNode *n = nodes_[oldest];
  byHash_.remove(detail::fnv1a64(n->text));
  nodes_.remove_at(oldest, true);
  PathPtr drop(n); /* releases the cache's reference */
}

void PathCache::setCapacity(int capacity)
{
  capacity_ = capacity < 1 ? 1 : capacity;
  while (size() > capacity_) {
    evictOne();
  }
}

void PathCache::clear()
{
  for (detail::PathNode *n : nodes_) {
    PathPtr drop(n); /* releases the cache's reference */
  }
  nodes_.clear();
  byHash_.clear();
}

Status PathCache::validate() const
{
  if (size() > capacity_ || int(byHash_.size()) != size()) {
    return Status::error(StatusCode::Internal, "PathCache: size and index disagree");
  }
  for (const detail::PathNode *n : nodes_) {
    detail::PathNode *const *found = byHash_.lookup_ptr(detail::fnv1a64(n->text));
    if (!found || *found != n || n->refs.load() < 1) {
      return Status::error(StatusCode::Internal, "PathCache: a node is not indexed");
    }
  }
  return Status();
}

} // namespace coredcc
