#pragma once

/** \file
 * The commands of scenes and objects. They work on the Context's scene. Pointer inputs are blocks.
 *
 * Those that take several objects work on the scene's selection, or on the objects that the
 * optional String input `objects` names: block ids as text (parseBlockIds), in that order, with
 * repeated ones ignored. Bad text is InvalidArgument, an id without a Live block NotFound, and an
 * object outside the scene FailedPrecondition.
 */

#include "coredcc/command/command.h"
#include "coredcc/lib/pin.h"
#include "coredcc/scene/scene.h"

namespace coredcc {

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
 * with neither selects nothing. ALL leaves out hidden and unselectable objects; naming one that is
 * unselectable fails. */
inline constexpr const char *kSelectCommand = "core.scene.select";
/** `type` (a transform type's name), `name`, `location` (in the world) and `parent`, which must
 * be in the scene. Outputs `object`. */
inline constexpr const char *kAddObjectCommand = "core.object.add";

/* Transforms of `objects` (transform_commands.cc). With `keepWorld`, they stay where they are:
 * exactly, or as well as their type can, which the output `approximate` counts. */

/** `parent` (null for none), in the scene and not below one of the objects, and `keepWorld`. */
inline constexpr const char *kSetParentCommand = "core.object.setParent";
/** `type` (a name), and `keepWorld`. */
inline constexpr const char *kSetTransformTypeCommand = "core.object.setTransformType";
/** `mode`: an item of the TRS type's rotationMode. The rotation stays. TRS objects only. */
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
  struct Stored {
    ChannelBlock channels;
    TypeId type;
    BlockId parent;
  };
  struct Change {
    BlockId object;
    bool hasChannels = false;
    bool hasType = false;
    bool hasParent = false;
    Stored before;
    Stored after;
  };
  static Status apply(Context &ctx, const Change &c, const Stored &values);
  Status takeBack(Context &ctx);

  litestl::util::Vector<Change> changes_;
  LibraryRef lib_;
};

/**
 * The instance of kApplyDeltaCommand and kDragDeltaCommand. A tool begins the second one, calls
 * `update` with the total delta since then as often as it likes, and commits or cancels:
 *
 *     executor.beginInteractive(ctx, def, def.newInputs());
 *     executor.interactiveCommand()->as<ApplyDelta>()->update(ctx, delta);
 */
class ApplyDelta {
 public:
  /** Applies `delta` to the objects as they were at the start. All or nothing. */
  Status update(Context &ctx, const TransformDelta &delta);

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
  };
  litestl::util::Vector<Base> bases_;
  double time_ = 0.0;
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
/** Those, or the selected ones if it isn't given: at least one. */
Result<litestl::util::Vector<BlockHandle>> targetObjects(Context &ctx);
/** A Pointer input's Live block; a null handle for a null pointer. */
Result<BlockHandle> blockOf(Context &ctx, const char *input);
/** A block as a Pointer value. */
PropValue pointerTo(const Library &lib, BlockHandle h);
/** The status, with a block's name in front. */
Status about(const Library &lib, BlockHandle h, const Status &s);
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
