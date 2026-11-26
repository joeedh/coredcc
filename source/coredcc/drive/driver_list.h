#pragma once

/** \file
 * The document's list of driver graphs: a block of type kDriverListType ("Drivers").
 *
 * - A graph in a list drives the whole document (SceneDrivers). Scenes don't list graphs,
 *   so duplicating a scene doesn't make a second driver.
 * - The commands edit the document's list (`DriverList::find`), and make one when there
 *   is none. An append can bring a second: every list is read, a graph that two lists
 *   name counts once, and removeGraph takes it out of every list.
 * - A duplicate of a list is empty, so copying a list with its graphs drives nothing
 * twice.
 * - The cells are OptStrong: a listed graph is kept and saved. The block is a GC root.
 * - In files, the payload's field `graphs`; unknown fields are kept.
 */

#include "coredcc/lib/library.h"
#include "coredcc/scene/payload_fields.h"

#include "litestl/util/vector.h"

namespace coredcc {

class ApiStruct;

inline constexpr const char *kDriverListType = "coredcc.DriverList";

class DriverList {
public:
  /** The list of a block of the type, or null. */
  static DriverList *of(const Library &lib, BlockHandle h);
  /** The document's list, which the commands edit: of the Live list blocks, a Root one
   * (the document's own) before others (appended or duplicated ones), then the one with
   * the least id. Null if none. */
  static BlockHandle find(const Library &lib);
  /** Every block of the type, in any state, by id: the order SceneDrivers reads them in.
   */
  static litestl::util::Vector<BlockHandle> allLists(const Library &lib);
  /** The Live graphs of every Live list, in that order, each once: those that drive. */
  static litestl::util::Vector<BlockHandle> allGraphs(const Library &lib);

  /** The Live blocks that the cells name, in order, each once. Valid until something
   * changes. Nothing checks that they are graphs: SceneDrivers reports those that aren't.
   */
  const litestl::util::Vector<BlockHandle> &graphs(const Library &lib) const;
  /** The cells, with those that aren't Live. */
  const litestl::util::Vector<RefCell> &cells() const
  {
    return cells_;
  }
  /** Puts a cell to `id` at `index` (-1 or past the end: last). NotFound for no record,
   * AlreadyExists if a cell names it. */
  Status insert(Library &lib, const BlockId &id, int index = -1);
  /** The same without the AlreadyExists check: for undo, which brings back what was. */
  Status insertCell(Library &lib, const BlockId &id, int index);
  /** Removes the cells that name `id`. Returns the first one's place, or NotFound. */
  Result<int> remove(Library &lib, const BlockId &id);

  void write(BlockWriter &out) const;
  void readFrom(BlockReader &in);
  DriverList *duplicate() const;
  uint64_t memSize() const;
  template <class Fn> void forEachCell(Fn &&fn)
  {
    for (RefCell &c : cells_) {
      fn(c);
    }
    for (BlockEntry &e : fields_) {
      for (RefCell &c : e.refs) {
        fn(c);
      }
    }
  }

private:
  litestl::util::Vector<RefCell> cells_;
  litestl::util::Vector<BlockEntry> fields_;
  mutable litestl::util::Vector<BlockHandle> live_;
  mutable uint64_t liveEpoch_ = ~uint64_t(0);
  mutable uint64_t liveGeneration_ = ~uint64_t(0);
};

/** A list block named "Drivers", a GC root. */
Result<BlockHandle> newDriverList(Library &lib);

/** Gives a host's root struct the read-only collection `driverGraphs`: the graphs of
 * every list, each once (DriverList::allGraphs), empty when there is no list. It looks at
 * every block on each read. The commands change it. */
void addDriverGraphsMember(ApiStruct &root);

namespace detail {
/** The block type and its struct; idempotent. Part of registerDriveTypes. */
Status registerDriverListType(Runtime &rt);
} // namespace detail

} // namespace coredcc
