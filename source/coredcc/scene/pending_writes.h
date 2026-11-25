#pragma once

#include "coredcc/lib/pin.h"
#include "coredcc/scene/transform.h"

#include "litestl/util/vector.h"

namespace coredcc {

class Context;

/** What an undo record stores of an object. */
struct StoredObject {
  ChannelBlock channels;
  TypeId type;
  BlockId parent;
};

/** Which of it. */
struct StoredFields {
  bool channels = false;
  bool type = false;
  bool parent = false;
};

namespace detail {

/** Stores the fields in a Live object: the parent before the rest, and a type with its channels. */
Status writeStored(Context &ctx, BlockHandle h, const StoredFields &fields, const StoredObject &v);

/**
 * What an undo, a redo or a drag would have written to objects that aren't Live: their hosts took them
 * away. It is written when they are Live again. A Runtime extension.
 */
class PendingWrites {
 public:
  static constexpr const char *kExtName = "coredcc.PendingWrites";
  explicit PendingWrites(Runtime *) {}

  /** In place of what waits for the same fields of it. */
  void add(Library &lib, const BlockId &object, const StoredFields &fields, const StoredObject &v);
  /** Writes what waits for an object, if it is Live; later, if nothing can be written now. An error
   * becomes the sticky one, if there is none. */
  void flush(Library &lib, const BlockId &object);
  /** The same for all objects of a Library. */
  void flush(Library &lib);
  int count(const Library &lib) const;

 private:
  struct Entry {
    BlockId object;
    StoredFields fields;
    StoredObject values;
  };
  struct PerLibrary {
    LibraryRef lib;
    litestl::util::Vector<Entry> entries;
  };
  PerLibrary *find(const Library &lib);

  litestl::util::Vector<PerLibrary> libraries_;
};

} // namespace detail
} // namespace coredcc
