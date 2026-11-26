#pragma once

/** \file
 * The commands of scenes and objects. They work on the Context's scene. Pointer inputs
 * are blocks.
 *
 * Those that take several objects work on the scene's selection, or on the objects that
 * the optional String input `objects` names: block ids as text (parseBlockIds), in that
 * order, with repeated ones ignored. Bad text is InvalidArgument, an id without a Live
 * block NotFound, and an object outside the scene FailedPrecondition.
 *
 * Objects that are hidden or unselectable by now stay selected, but the commands don't
 * work on them because of that: nobody sees them, or can take them out of the selection.
 * `objects` may name them.
 *
 * An object that a transform command can't work on (its parent's matrix has no inverse,
 * its transform type isn't there, it isn't Transformable, ...) is left out, and counted
 * in the output `skipped`. The command fails, with the first one's error, when there is
 * none that it can work on.
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/pending_writes.h"
#include "coredcc/scene/scene.h"

#include <span>

namespace coredcc {

namespace detail {
class StateCache;
}

/* Membership (membership_commands.cc). Link, unlink and add also drop members without a
 * record. */

/** `name`. A new scene; it doesn't become the Context's. Outputs `scene`. */
inline constexpr const char *kNewSceneCommand = "core.scene.new";
/** `object`, or `objects`: adds them at the end. */
inline constexpr const char *kLinkSceneCommand = "core.scene.link";
/** Removes `objects`. Undo puts each behind the one it was behind. Outputs `count`. */
inline constexpr const char *kUnlinkSceneCommand = "core.scene.unlink";
/** `object`, or null for none. */
inline constexpr const char *kSetSceneActiveCommand = "core.scene.setActive";
/** `mode` (SET, ADD, REMOVE, TOGGLE, ALL, NONE), and `object` or `objects` for the first
 * four; SET with neither selects nothing. Unselectable objects are left out, and for ALL
 * hidden ones too. Outputs `skipped`: how many of those that were named were left out. */
inline constexpr const char *kSelectSceneCommand = "core.scene.select";
/** `type` (a transform type's name), `name`, `location`, `rotation` (a quaternion) and
 * `scale` (in the world, as well as the type can), `parent`, which must be in the scene,
 * and `data`. With `select`, it is what is selected, and active. Outputs `object`. */
inline constexpr const char *kNewObjectCommand = "core.object.new";
/** Copies of `objects` where they are, in the scene, at the end. The copies' parents are
 * copies if those are copied too. `children`: with the members below them. `linkData`:
 * they have the same data blocks; otherwise copies of them. `select`: the copies are
 * selected instead, and the active object's copy is active. Outputs `objects` (the
 * copies, as text, in order) and `count`. To drag them, it is followed by
 * core.object.dragDelta. */
inline constexpr const char *kDuplicateObjectCommand = "core.object.duplicate";
/** A copy of `scene` (null for the Context's), a GC root, under `name`, or its own if
 * that is empty. `objects`: with copies of its members; otherwise the copy has the same
 * ones. `linkData`: as above. It doesn't become the Context's. Outputs `scene`. */
inline constexpr const char *kDuplicateSceneCommand = "core.scene.duplicate";

/* Transforms of `objects` (transform_commands.cc). With `keepWorld`, they stay where they
 * are: exactly, or as well as their type can, which the output `approximate` counts. So
 * do the objects below them, except those that depend on more of their parent than its
 * matrix: FBX's RrSs and Rrs use its rotation and scale. Those that move, for that or
 * because one above them is only near its place, are counted in `approximate` too. */

/** `parent` (null for none), in the scene and not below one of the objects, and
 * `keepWorld`. */
inline constexpr const char *kSetObjectParentCommand = "core.object.setParent";
/** `type` (a name), and `keepWorld`. Objects that have the type stay as they are. */
inline constexpr const char *kSetObjectTransformTypeCommand =
    "core.object.setTransformType";
/** `mode`: an item of the TRS type's rotationMode. The rotation stays. Others than TRS
 * objects are skipped. */
inline constexpr const char *kSetObjectRotationModeCommand =
    "core.object.setRotationMode";
/** A TransformDelta (`kind`, `basis`, `pivot`, `vector`, `axis`, `angle`). Objects below
 * another of them move with it, and aren't changed themselves. With a `gesture`
 * (beginGesture), one that follows another of the same gesture, on the same objects, is
 * one step with it: for the arrow keys. Outputs `locked` and `driven`: see ApplyDelta. */
inline constexpr const char *kApplyObjectDeltaCommand = "core.object.applyDelta";
/** The same as an Interactive command: see ApplyDelta. */
inline constexpr const char *kDragObjectDeltaCommand = "core.object.dragDelta";
/** `matrices`: 16 numbers, column by column, for each of `objects`, which must be given
 * and name none twice: where they are to be, parents first. `mode`: EXACT, or CLOSEST,
 * which the output `approximate` counts. Locked channels stay. */
inline constexpr const char *kSetObjectWorldCommand = "core.object.setWorld";
/** `flags` (HIDDEN, UNSELECTABLE), and whether they are `on`. */
inline constexpr const char *kSetObjectFlagsCommand = "core.object.setFlags";
/** `channels`: names with spaces between, of channels or of one component ("location.0");
 * all of them if it is empty. `on`: whether they are locked. See ObjectInfo::locks. */
inline constexpr const char *kSetObjectLocksCommand = "core.object.setLocks";
/** `keepChildrenWorld`: first, Live children that stay are given to their nearest
 * ancestor that stays. Then one core.block.delete for all. Outputs `count` and
 * `approximate`. */
inline constexpr const char *kDeleteObjectCommand = "core.object.delete";

/** Registers the scene's types and commands, and the block commands (idempotent). */
Status registerSceneCommands(Runtime &rt);

/**
 * What a command changes about objects, for its undo. Objects are found by id. Undo and
 * redo write stored values: nothing is computed again. All of it is FailedPrecondition in
 * a Context of another Library than the first write's.
 *
 * An object that isn't Live any more when it is written again (its host took it away)
 * isn't an error: it gets the value when it is Live again, or the last one, if there are
 * several by then.
 */
class ObjectChanges {
public:
  struct Values {
    const ChannelBlock *channels = nullptr;
    /** With channels of the type. */
    const TypeId *type = nullptr;
    const BlockId *parent = nullptr;
    const ObjectFlags *flags = nullptr;
    const uint64_t *locks = nullptr;
  };
  /** Writes what isn't null. A failure takes everything back, also of earlier calls. */
  Status write(Context &ctx, BlockHandle object, const Values &values);
  /** Writes other channels to the i-th object, as if they had been the first. */
  Status rewrite(Context &ctx, int i, const ChannelBlock &channels);
  /** Errors become the sticky error; the rest is still undone. */
  void undo(Context &ctx);
  /** All or nothing. */
  Status redo(Context &ctx);
  /** Takes over what `next` wrote, if that is of the same objects and fields, and began
   * where this ended. False, with nothing changed, if it isn't. */
  bool foldFrom(const ObjectChanges &next);
  /** Whether a channel that was written is different now. */
  bool channelsChanged() const;

  int size() const
  {
    return int(changes_.size());
  }
  /** The i-th object's channels: as they were, and as they are written. */
  const ChannelBlock &before(int i) const
  {
    return changes_[i].before.channels;
  }
  const ChannelBlock &after(int i) const
  {
    return changes_[i].after.channels;
  }
  uint64_t byteSize() const;

private:
  struct Change {
    BlockId object;
    StoredFields fields;
    StoredObject before;
    StoredObject after;
  };
  static Status apply(Context &ctx, const Change &c, const StoredObject &values);
  Status takeBack(Context &ctx);

  litestl::util::Vector<Change> changes_;
  LibraryRef lib_;
};

/**
 * The instance of kApplyObjectDeltaCommand and kDragObjectDeltaCommand. A tool begins the
 * second one, calls `update` with the total delta since then as often as it likes, and
 * commits or cancels:
 *
 *     executor.beginInteractive(ctx, def, def.newInputs());
 *     executor.updateInteractive(ctx, [&](Context &c, Command &cmd) {
 *       return cmd.as<ApplyDelta>()->update(c, delta);
 *     });
 *
 * A tool that began one for a click and has nothing `changed` cancels it, so that there
 * is no empty step. One that gives each object a delta of its own asks `startState` and
 * `startPivot` for what those depend on. Its record's inputs don't tell what it did then:
 * redo writes the stored values, but running the command with the same inputs again does
 * nothing.
 */
class ApplyDelta {
public:
  /** Applies `delta` to the objects as they were at the start. One that it can't be
   * applied to stays as it is (`skipped`); it fails, and changes nothing, when that is
   * all of them, or when one can't be written. */
  Status update(Context &ctx, const TransformDelta &delta);
  /** The same with a delta for each object, as `object` has them: snapping each to a
   * grid, turning each about itself, or moving some less than others. InvalidArgument for
   * another number of them. */
  Status update(Context &ctx, std::span<const TransformDelta> deltas);

  /** The objects that it changes: those that aren't below another, and were usable at the
   * start. */
  int objectCount() const
  {
    return int(bases_.size());
  }
  BlockHandle object(int i) const
  {
    return bases_[i].object;
  }
  /** The state of the object's parent at the start. */
  const TransformState &startParent(int i) const
  {
    return bases_[i].parent;
  }
  /** The state of the object at the start, and the point that it turned about. */
  Result<TransformState> startState(const Context &ctx, int i) const;
  Result<double3> startPivot(const Context &ctx, int i) const;
  /** Whether an object's channels are others than at the start. */
  bool changed() const
  {
    return changes_.channelsChanged();
  }

  /** How many objects the last `update` left out, with those that were left out at the
   * start. */
  int skipped() const
  {
    return skipped_;
  }
  /** How many objects the last `update` couldn't move all the way: locked channels were
   * in the way, or driven ones. They are moved by the others. */
  int locked() const
  {
    return locked_;
  }
  int driven() const
  {
    return driven_;
  }

  Status canRun(Context &ctx) const;
  Status exec(Context &ctx);
  void undo(Context &ctx)
  {
    changes_.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return changes_.redo(ctx);
  }
  uint64_t byteSize() const
  {
    return sizeof(*this) + changes_.byteSize();
  }
  void discard()
  {
  }
  uint64_t foldKey() const;
  bool foldFrom(Command next);
  static litestl::binding::types::Struct<ApplyDelta> *defineBindings();

private:
  struct Base {
    BlockHandle object;
    TypeId type;
    TransformState parent;
    /* At the start: what its solutions can't change. */
    ChannelOverrides overrides;
    uint64_t locks = 0;
    ChannelBlock solved; /* for exec's delta, until it is written */
    FixedChannels fixed;

    EvalContext ctx(const ChannelBlock &channels, double time) const
    {
      const EvalContext c =
          overrides.any() ? EvalContext(channels, time, overrides.values, overrides.mask)
                          : EvalContext(channels, time);
      return c.withLocks(locks);
    }
  };
  /* `deltas` has one for all, or one for each. */
  Status updateWith(Context &ctx, std::span<const TransformDelta> deltas);
  Result<Base> baseOf(const Library &lib,
                      detail::StateCache &states,
                      BlockHandle h,
                      const TransformDelta &delta) const;

  litestl::util::Vector<Base> bases_;
  double time_ = 0.0;
  /* Left out at the start, and by the last update. */
  int unusable_ = 0;
  int skipped_ = 0;
  int locked_ = 0;
  int driven_ = 0;
  int64_t gesture_ = 0;
  ObjectChanges changes_;
};

namespace detail {

/** The Context's Library and scene, or why not. */
struct SceneOf {
  Library *lib = nullptr;
  Scene *scene = nullptr;
  BlockHandle handle;
};
Result<SceneOf> sceneOf(Context &ctx);
/** The input `objects`. */
PropertyDef objectsInput();
/** The Live blocks it names, each once. */
Result<litestl::util::Vector<BlockHandle>> blocksNamed(Context &ctx);
/** The same; they must be in the scene. */
Result<litestl::util::Vector<BlockHandle>> objectsOf(Context &ctx);
/** Those, or the selected ones if it isn't given, without hidden and unselectable ones:
 * at least one. */
Result<litestl::util::Vector<BlockHandle>> targetObjects(Context &ctx);
/** A Pointer input's Live block; a null handle for a null pointer. */
Result<BlockHandle> blockOf(Context &ctx, const char *input);
/** Its ObjectFlags; none for a block that isn't Transformable. */
ObjectFlags flagsOf(const Library &lib, BlockHandle h);
/** A block as a Pointer value. */
PropValue pointerTo(const Library &lib, BlockHandle h);
/** The status, with a block's name in front if it doesn't begin with that. */
Status about(const Library &lib, BlockHandle h, const Status &s);
/** Objects that a command leaves out, and why the first one was. */
class Skips {
public:
  /** The error, if it isn't one for leaving an object out: an exception of a script, say.
   */
  Status add(const Library &lib, BlockHandle h, const Status &why);
  int skipCount() const
  {
    return count_;
  }
  /** The first error, when there is one and nothing was done. */
  Status outcome(int done) const
  {
    return done > 0 ? Status() : first_;
  }
  /** Sets the output `skipped`. */
  void output(Context &ctx) const;

private:
  int count_ = 0;
  Status first_;
};

Status registerMembershipCommands(Runtime &rt);
Status registerDuplicateCommands(Runtime &rt);
Status registerTransformCommands(Runtime &rt);

/** Channels of `type` at `world`: exactly, or as well as they can (which adds to
 * `approximate`). */
Result<ChannelBlock> solveKeeping(const TransformType &type,
                                  const EvalContext &current,
                                  const mat4d &world,
                                  const TransformState &parent,
                                  int &approximate);

} // namespace detail

} // namespace coredcc
