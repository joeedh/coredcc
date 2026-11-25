#pragma once

/** \file
 * The commands of scenes and objects. They work on the Context's scene. Pointer inputs are blocks.
 *
 * Those that take several objects work on the scene's selection, or on the objects that the
 * optional String input `objects` names: block ids as text (parseBlockIds), in that order, with
 * repeated ones ignored. Bad text is InvalidArgument, an id without a Live block NotFound, and an
 * object outside the scene FailedPrecondition.
 *
 * Objects that are hidden or unselectable by now stay selected, but the commands don't work on them
 * because of that: nobody sees them, or can take them out of the selection. `objects` may name them.
 *
 * An object that a transform command can't work on (its parent's matrix has no inverse, its transform
 * type isn't there, it isn't Transformable, ...) is left out, and counted in the output `skipped`. The
 * command fails, with the first one's error, when there is none that it can work on.
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/pending_writes.h"
#include "coredcc/scene/scene.h"

namespace coredcc {

namespace detail {
class StateCache;
}

/* Membership (membership_commands.cc). Link, unlink and add also drop members without a
 * record. */

/** `name`. A new scene; it doesn't become the Context's. Outputs `scene`. */
inline constexpr const char *kNewSceneCommand = "core.scene.new";
/** `object`: adds it at the end. */
inline constexpr const char *kLinkCommand = "core.scene.link";
/** Removes `objects`. Undo puts each behind the one it was behind. Outputs `count`. */
inline constexpr const char *kUnlinkCommand = "core.scene.unlink";
/** `object`, or null for none. */
inline constexpr const char *kSetActiveCommand = "core.scene.setActive";
/** `mode` (SET, ADD, REMOVE, TOGGLE, ALL, NONE), and `object` or `objects` for the first four; SET
 * with neither selects nothing. Unselectable objects are left out, and for ALL hidden ones too.
 * Outputs `skipped`: how many of those that were named were left out. */
inline constexpr const char *kSelectCommand = "core.scene.select";
/** `type` (a transform type's name), `name`, `location` (in the world) and `parent`, which must
 * be in the scene. Outputs `object`. */
inline constexpr const char *kAddObjectCommand = "core.object.add";

/* Transforms of `objects` (transform_commands.cc). With `keepWorld`, they stay where they are:
 * exactly, or as well as their type can, which the output `approximate` counts. So do the objects
 * below them, except those that depend on more of their parent than its matrix: FBX's inherit types
 * RrSs and Rrs use its rotation and scale. Those that move, for that or
 * because one above them is only near its place, are counted in `approximate` too. */

/** `parent` (null for none), in the scene and not below one of the objects, and `keepWorld`. */
inline constexpr const char *kSetParentCommand = "core.object.setParent";
/** `type` (a name), and `keepWorld`. Objects that have the type stay as they are. */
inline constexpr const char *kSetTransformTypeCommand = "core.object.setTransformType";
/** `mode`: an item of the TRS type's rotationMode. The rotation stays. Others than TRS objects
 * are skipped. */
inline constexpr const char *kSetRotationModeCommand = "core.object.setRotationMode";
/** A TransformDelta (`kind`, `basis`, `pivot`, `vector`, `axis`, `angle`). Objects below another
 * of them move with it, and aren't changed themselves. */
inline constexpr const char *kApplyDeltaCommand = "core.object.applyDelta";
/** The same as an Interactive command: see ApplyDelta. */
inline constexpr const char *kDragDeltaCommand = "core.object.dragDelta";
/** `keepChildrenWorld`: first, Live children that stay are given to their nearest ancestor that
 * stays. Then one core.block.delete for all. Outputs `count` and `approximate`. */
inline constexpr const char *kDeleteObjectsCommand = "core.object.delete";

/** Registers the scene's types and commands, and the block commands (idempotent). */
Status registerSceneCommands(Runtime &rt);

/**
 * What a command changes about objects, for its undo. Objects are found by id. Undo and redo write
 * stored values: nothing is computed again. All of it is FailedPrecondition in a Context of another
 * Library than the first write's.
 *
 * An object that isn't Live any more when it is written again (its host took it away) isn't an error:
 * it gets the value when it is Live again, or the last one, if there are several by then.
 */
class ObjectChanges {
 public:
  struct Values {
    const ChannelBlock *channels = nullptr;
    /** With channels of the type. */
    const TypeId *type = nullptr;
    const BlockId *parent = nullptr;
  };
  /** Writes what isn't null. A failure takes everything back, also of earlier calls. */
  Status write(Context &ctx, BlockHandle object, const Values &values);
  /** Writes other channels to the i-th object, as if they had been the first. */
  Status rewrite(Context &ctx, int i, const ChannelBlock &channels);
  /** Errors become the sticky error; the rest is still undone. */
  void undo(Context &ctx);
  /** All or nothing. */
  Status redo(Context &ctx);

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
  uint64_t memSize() const;

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
 * The instance of kApplyDeltaCommand and kDragDeltaCommand. A tool begins the second one, calls
 * `update` with the total delta since then as often as it likes, and commits or cancels:
 *
 *     executor.beginInteractive(ctx, def, def.newInputs());
 *     executor.updateInteractive(ctx, [&](Context &c, Command &cmd) {
 *       return cmd.as<ApplyDelta>()->update(c, delta);
 *     });
 */
class ApplyDelta {
 public:
  /** Applies `delta` to the objects as they were at the start. One that it can't be applied to stays
   * as it is (`skipped`); it fails, and changes nothing, when that is all of them, or when one can't be
   * written. */
  Status update(Context &ctx, const TransformDelta &delta);
  /** How many objects the last `update` left out, with those that were left out at the start. */
  int skipped() const
  {
    return skipped_;
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
  uint64_t memSize() const
  {
    return sizeof(*this) + changes_.memSize();
  }
  void discard() {}
  static litestl::binding::types::Struct<ApplyDelta> *defineBindings();

 private:
  struct Base {
    BlockHandle object;
    TypeId type;
    TransformState parent;
    ChannelBlock solved; /* for exec's delta, until it is written */
  };
  Result<Base> baseOf(const Library &lib,
                      detail::StateCache &states,
                      BlockHandle h,
                      const TransformDelta &delta) const;

  litestl::util::Vector<Base> bases_;
  double time_ = 0.0;
  /* Left out at the start, and by the last update. */
  int unusable_ = 0;
  int skipped_ = 0;
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
/** The objects it names. */
Result<litestl::util::Vector<BlockHandle>> objectsOf(Context &ctx);
/** Those, or the selected ones if it isn't given, without hidden and unselectable ones: at least
 * one. */
Result<litestl::util::Vector<BlockHandle>> targetObjects(Context &ctx);
/** A Pointer input's Live block; a null handle for a null pointer. */
Result<BlockHandle> blockOf(Context &ctx, const char *input);
/** Its ObjectFlags; none for a block that isn't Transformable. */
ObjectFlags flagsOf(const Library &lib, BlockHandle h);
/** A block as a Pointer value. */
PropValue pointerTo(const Library &lib, BlockHandle h);
/** The status, with a block's name in front. */
Status about(const Library &lib, BlockHandle h, const Status &s);
/** Objects that a command leaves out, and why the first one was. */
class Skips {
 public:
  /** The error, if it isn't one for leaving an object out: an exception of a script, say. */
  Status add(const Library &lib, BlockHandle h, const Status &why);
  int count() const
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
Status registerTransformCommands(Runtime &rt);

/** Channels of `type` at `world`: exactly, or as well as they can (which adds to
 * `approximate`). */
Result<ChannelBlock> solveKeeping(const TransformType &type,
                                  const EvalCtx &current,
                                  const mat4d &world,
                                  const TransformState &parent,
                                  int &approximate);

} // namespace detail

} // namespace coredcc
