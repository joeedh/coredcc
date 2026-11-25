#pragma once

/** \file
 * What in an instance depends on what, backwards: which vertices an edit touches. Internal
 * to graph/.
 */

#include "coredcc/base/block_id.h"
#include "coredcc/graph/trace.h"

#include "litestl/util/map.h"

#include <cstdint>

namespace coredcc::graph {

struct Frame;
struct KernelState;

/** A vertex of the walk: an input socket, or a kernel node with its outputs. Outputs of
 * groups and interface inputs need none: each leads to exactly one other vertex. */
struct Vertex {
  Frame *frame = nullptr;
  /** In GraphTopology::inputs, or ::nodes for a kernel. */
  int index = -1;
  bool kernel = false;
  bool operator==(const Vertex &) const = default;
};

/** An input socket in the list of those that read one vertex, by InputState::source. */
struct InputLink {
  InputLink() = default;
  ~InputLink();
  InputLink(const InputLink &) = delete;
  InputLink &operator=(const InputLink &) = delete;

  Frame *frame = nullptr;
  /** The list it is in; null if none. */
  InputLink **head = nullptr;
  InputLink *next = nullptr;
  InputLink *prev = nullptr;
  /** The list of those that read this one. */
  InputLink *consumers = nullptr;

  /** Its place in `frame`. */
  int index() const;
  /** Puts it into another list, or none. */
  void move(InputLink **list);
  /** Takes every link out of a list that goes. */
  static void detachAll(InputLink *&list);
};

/** A kernel in the list of those that used one block. */
struct BlockLink {
  KernelState *kernel = nullptr;
  /** Canonical. */
  BlockId block;
  BlockLink *next = nullptr;
  BlockLink *prev = nullptr;
};

/** The frames by their def, the kernels by the blocks they used, and the foreach nodes. Of
 * the frames outside of elements. */
class Dependents {
 public:
  void addFrame(Frame &f);
  void removeFrame(Frame &f);
  /** Notes what an input reads. */
  static void setSource(Frame &f, int input, const Vertex &source, KernelState *kernel);
  /** Notes the blocks in a kernel's trace. */
  void setBlocks(KernelState &k, class ClientRegistry &clients);
  void removeKernel(KernelState &k);

  /** The first frame of a def (Frame::nextOfDef), or null. */
  Frame *framesOf(uint64_t defSerial) const
  {
    Frame *const *f = frames_.lookup_ptr(defSerial);
    return f ? *f : nullptr;
  }
  /** The first kernel of a block, or null. */
  BlockLink *kernelsOf(BlockId canonical) const
  {
    BlockLink *const *l = blocks_.lookup_ptr(canonical);
    return l ? *l : nullptr;
  }

  litestl::util::Map<KernelState *, bool> foreachs;

 private:
  void unlink(BlockLink &l);

  litestl::util::Map<uint64_t, Frame *> frames_;
  litestl::util::Map<BlockId, BlockLink *> blocks_;
};

} // namespace coredcc::graph
