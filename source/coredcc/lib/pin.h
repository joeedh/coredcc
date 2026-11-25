#pragma once

/** \file
 * LibraryRef and LibraryPin: holding on to a Library and its blocks from something that
 * may outlive the Library (graph payloads, evaluated state).
 */

#include "coredcc/lib/library.h"

#include <memory>
#include <utility>

namespace coredcc {

namespace detail {
/** Shared by a Library and the refs to it; the Library clears it as its destructor
 * starts. */
struct LibraryToken {
  Library *lib = nullptr;
};
} // namespace detail

/** A weak reference to a Library. Copyable; from Library::ref(). */
class LibraryRef {
public:
  LibraryRef() = default;
  /** Null once the Library is being destroyed (its providers' `destroy` already see
   * null), and for a default-constructed ref. */
  Library *get() const
  {
    return token_ ? token_->lib : nullptr;
  }

private:
  friend class Library;
  explicit LibraryRef(std::shared_ptr<detail::LibraryToken> token)
      : token_(std::move(token))
  {
  }
  std::shared_ptr<detail::LibraryToken> token_;
};

/**
 * Pins a block for its own lifetime. Move-only; copy() pins again. It may outlive the
 * Library, and then does nothing. Main thread only: a payload released on a worker must
 * hand its pin back.
 */
class LibraryPin {
public:
  LibraryPin() = default;
  LibraryPin(Library &lib, BlockHandle h, PinKind kind = PinKind::Host);
  LibraryPin(LibraryPin &&b) noexcept
      : ref_(std::exchange(b.ref_, LibraryRef())),
        handle_(std::exchange(b.handle_, BlockHandle())), kind_(b.kind_)
  {
  }
  LibraryPin &operator=(LibraryPin &&b) noexcept;
  LibraryPin(const LibraryPin &) = delete;
  LibraryPin &operator=(const LibraryPin &) = delete;
  ~LibraryPin()
  {
    reset();
  }

  LibraryPin copy() const;
  /** Unpins now. */
  void reset();

  /** The Library, or null once it is being destroyed. */
  Library *library() const
  {
    return ref_.get();
  }
  BlockHandle handle() const
  {
    return handle_;
  }
  /**
   * Whether memory inside the block's data is still there: the Library is alive and the
   * block is Live, or was when it was deleted (InHistory, Doomed). False for a Detached
   * block (the host freed its data, whatever the pins) and for a placeholder.
   */
  bool dataAlive() const;

private:
  LibraryRef ref_;
  BlockHandle handle_;
  PinKind kind_ = PinKind::Host;
};

} // namespace coredcc
