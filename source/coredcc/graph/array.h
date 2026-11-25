#pragma once

/** \file
 * Array: a list whose length is set once. Internal to graph/.
 */

#include <memory>
#include <utility>

namespace coredcc::graph {

/** Elements on the heap, as many as `reset` said and no more. 16 bytes when empty, where
 * a Vector has room for elements. Their addresses don't change. */
template <class T> class Array {
public:
  Array() = default;
  Array(Array &&b) noexcept : data_(std::move(b.data_)), size_(std::exchange(b.size_, 0))
  {
  }
  Array &operator=(Array &&b) noexcept
  {
    data_ = std::move(b.data_);
    size_ = std::exchange(b.size_, 0);
    return *this;
  }

  /** Drops what there is, and makes `size` new ones. */
  void reset(int size = 0)
  {
    data_ = nullptr;
    size_ = size;
    if (size > 0) {
      data_ = std::make_unique<T[]>(size_t(size));
    }
  }
  int size() const
  {
    return size_;
  }
  T &operator[](int i)
  {
    return data_[i];
  }
  const T &operator[](int i) const
  {
    return data_[i];
  }
  T *begin()
  {
    return data_.get();
  }
  T *end()
  {
    return data_.get() + size_;
  }
  const T *begin() const
  {
    return data_.get();
  }
  const T *end() const
  {
    return data_.get() + size_;
  }

private:
  std::unique_ptr<T[]> data_;
  int size_ = 0;
};

} // namespace coredcc::graph
