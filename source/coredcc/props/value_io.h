#pragma once

/** \file
 * The binary form of PropValue and PropertyBag (files, clipboards, preferences). The wire
 * tags below and the layouts in value_io.cc are the file format: they never change
 * meaning.
 */

#include "coredcc/base/bytes.h"
#include "coredcc/base/name_table.h"
#include "coredcc/base/runtime.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/value.h"

#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace coredcc {

/** Value type tags as written. Their own constants, so PropValue::Type can change. */
enum class ValueTag : uint8_t {
  None = 0,
  Bool = 1,
  Int = 2,
  Float = 3,
  String = 4,
  Vec2 = 5,
  Vec3 = 6,
  Vec4 = 7,
  Mat4 = 8,
  Quat = 9,
  Enum = 10,
  Flags = 11,
  Pointer = 12,
};

ValueTag valueTag(PropValue::Type type);
/** Whether this build knows `tag`. */
bool isKnownValueTag(uint8_t tag);

/**
 * How ids are written. The default writes a block id as its client's name and 128 bits;
 * block payloads write an index into their ref table instead (lib/block_io.h).
 *
 * A name is written as a form byte: null, the name, or (when neither the Runtime nor the
 * table knows it) the raw id, so an unknown name never fails a save.
 */
class ValueCodec {
public:
  /** `names` holds the names only files know; null to keep none (ids whose names aren't
   * registered then lose them). Both must outlive the codec. */
  ValueCodec(const Runtime &rt, NameTable *names) : rt_(rt), names_(names)
  {
  }
  virtual ~ValueCodec() = default;

  virtual void writeBlockId(BytesWriter &out, const BlockId &id);
  virtual void readBlockId(BytesReader &in, BlockId &id);
  /** Called before a pointer's `n` key segments are read: fails `in` when they need more
   * memory than there is. */
  virtual void chargeKey(BytesReader &in, uint32_t n);

  void writeName(BytesWriter &out, IdDomain domain, uint64_t id) const;
  /** 0 for a null id. A name that hashes to an id the Runtime or the table has under
   * another name fails the read (InvalidArgument). */
  uint64_t readName(BytesReader &in, IdDomain domain);

private:
  const Runtime &rt_;
  NameTable *names_;
};

/** The most segments a pointer's key has in a file. Part of the format, version 1. */
inline constexpr uint32_t kMaxKeySegments = 1024;

/** A tag byte, then the body. Fails the writer for a Pointer that can't be read back:
 * with a negative handle in its key (InvalidArgument), which no path can address, or more
 * than kMaxKeySegments (OutOfRange). */
void writeValue(BytesWriter &out, const PropValue &v, ValueCodec &codec);
/** None after a failure, an unknown tag among them. */
PropValue readValue(BytesReader &in, ValueCodec &codec);

/** The body alone, for callers that write the tag themselves. */
void writeValueBody(BytesWriter &out, const PropValue &v, ValueCodec &codec);
PropValue readValueBody(BytesReader &in, ValueTag tag, ValueCodec &codec);

/** An entry of a stored bag that the bag wouldn't take. */
struct BagIssue {
  litestl::util::string name;
  Status why;
};
/** readBag reports at most this many issues. */
inline constexpr int kMaxBagIssues = 1000;

/**
 * Writes the entries that are set, by name. For data outside blocks (a block's bag goes
 * through BlockWriter, so its pointers are in the ref table). Enum and Flags are written
 * as keys (`EDIT`, `A|B`), or as numbers when a value has no item.
 */
void writeBag(BytesWriter &out, const PropertyBag &bag, ValueCodec &codec);
/** Sets the stored entries into `bag`, whose defs come from code. Entries it doesn't
 * take (unknown name or tag, a value that doesn't coerce) are issues, not failures. */
void readBag(BytesReader &in,
             PropertyBag &bag,
             ValueCodec &codec,
             litestl::util::Vector<BagIssue> &issues);

/** A value of `def` (not a dynamic one) as it is stored: an Enum or Flags as its keys
 * where it has them. */
PropValue storedValue(const PropertyDef &def, const PropValue &v);
/** The same for entry `i` of `bag`. */
PropValue storedBagValue(const PropertyBag &bag, int i);

} // namespace coredcc
