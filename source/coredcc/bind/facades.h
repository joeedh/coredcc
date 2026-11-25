#pragma once

/** \file
 * What scripts call: free functions over the C++ API, in shapes the binding system can
 * carry (P1f step 4 in docs/plans/2026-11-20-p1-application-spine.md).
 *
 * - Each facade is bound as `coredcc::<name>`. It takes `const litestl::util::string &`
 *   rather than `std::string_view`, and returns a Status, a value, or a result struct
 *   in place of a `Result<T>`.
 * - Misuse from a script (the wrong value type, an index out of range, a call that
 *   would reenter the history) is an error result. It never aborts.
 * - What is bound refuses strings with a NUL (bind/checked.h): strings, names and keys can't
 *   hold one.
 * - Facades never set the Runtime's sticky error: a failure is in what they return.
 *
 * **Inside a script command's slots** the facades that change the document are
 * limited by slot, and are Busy elsewhere:
 * - `setValue` and `invoke` only in `exec`, where they run as sub-commands;
 * - `writeValue` only in `exec`, `undo` and `redo`;
 * - `undo` and `redo` never;
 * - reads anywhere but in a graph kernel (graph_facades.h), where all that takes a Context is Busy.
 *
 * Outside any script slot (a script's top level) every facade is allowed.
 */

#include "coredcc/api/context.h"
#include "coredcc/base/status.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/value.h"

#include "litestl/util/string.h"

#include <cstdint>

namespace coredcc::script {

using litestl::util::string;

/* ---- result structs: a Status, and a value that is meaningful only when it is ok ---- */

struct BoolResult {
  Status status;
  bool value = false;
  static const litestl::binding::types::Struct<BoolResult> *defineBindings();
};
struct IntResult {
  Status status;
  int64_t value = 0;
  static const litestl::binding::types::Struct<IntResult> *defineBindings();
};
struct FloatResult {
  Status status;
  double value = 0;
  static const litestl::binding::types::Struct<FloatResult> *defineBindings();
};
struct StringResult {
  Status status;
  string value;
  static const litestl::binding::types::Struct<StringResult> *defineBindings();
};
struct ValueResult {
  Status status;
  PropValue value;
  static const litestl::binding::types::Struct<ValueResult> *defineBindings();
};
struct BagResult {
  Status status;
  PropertyBag value;
  static const litestl::binding::types::Struct<BagResult> *defineBindings();
};

/* ---- Status ---- */

/** An error Status. `Ok`, and a number that is no StatusCode, become InvalidArgument. */
Status statusError(StatusCode code, const string &message);
/** "Code: message (did you mean: a, b?)", or "Ok": the message with the suggestions. */
string statusText(const Status &s);

/* ---- PropValue: makers and checked readers ---- */

PropValue makeBool(bool v);
PropValue makeInt(int64_t v);
PropValue makeFloat(double v);
ValueResult makeString(const string &v);
PropValue makeVec2(double x, double y);
PropValue makeVec3(double x, double y, double z);
PropValue makeVec4(double x, double y, double z, double w);
/** Flags by their bits. */
PropValue makeFlags(int64_t bits);
PropValue makeNullPointer();
/** A Pointer from its text (api/ref_text.h). */
ValueResult makePointer(Context &ctx, const string &text);

/** The value, or InvalidArgument when `v` holds another type. */
BoolResult readBool(const PropValue &v);
IntResult readInt(const PropValue &v);
FloatResult readFloat(const PropValue &v);
StringResult readString(const PropValue &v);
/** Component `i` of a Float, vector, Mat4 or Quat; OutOfRange past componentCount(). */
FloatResult readComponent(const PropValue &v, int32_t i);
/** An Enum value's number. Write enums with makeString and the item's key. */
IntResult readEnum(const PropValue &v);
/** A Flags value's bits, as an int64 with the same bits. */
IntResult readFlags(const PropValue &v);
/** A Pointer's text (api/ref_text.h); "" for null. */
StringResult pointerText(Context &ctx, const PropValue &v);
/** A readable form for display and debugging (PropValue::toString): not an invocation
 * literal. */
string valueToString(const PropValue &v);

/* ---- PropertyBag: read-only ---- */

/** NotFound, with suggestions, for an unknown name. */
ValueResult bagGet(const PropertyBag &bag, const string &name);
int32_t bagSize(const PropertyBag &bag);
/** The name of entry `i`; OutOfRange past bagSize(). */
StringResult bagName(const PropertyBag &bag, int32_t i);

/* ---- the running command's bags: FailedPrecondition outside a command slot ---- */

ValueResult inputGet(Context &ctx, const string &name);
Status outputSet(Context &ctx, const string &name, const PropValue &value);
ValueResult outputGet(Context &ctx, const string &name);

/* ---- datapaths and commands ---- */

/** What `path` is (bind/meta.h's pathKindName), or the resolver's error, with suggestions. */
StringResult resolvePath(Context &ctx, const string &path);
/** pathMeta's JSON (bind/meta.h). */
StringResult pathMeta(Context &ctx, const string &path);
/** `v`, read from `path`, by name: an Enum's key, or a Flags' keys joined by '|'. NotFound for a
 * number without an item, InvalidArgument for another kind of value. */
StringResult valueKeys(Context &ctx, const string &path, const PropValue &v);
ValueResult getValue(Context &ctx, const string &path);
/**
 * An undoable set (setValueUndo): a command in the history, or a sub-command when
 * called from a command's exec. Needs an executor on `ctx`. Busy from any other slot of
 * a script command (undo, discard...): use writeValue there.
 */
Status setValue(Context &ctx, const string &path, const PropValue &value);
/** A new gesture, for setValueGesture. Not from a kernel. */
IntResult beginGesture(Context &ctx);
/** Takes back what the gesture's sets did, as far as they are the last commands. Not from a slot. */
Status cancelGesture(Context &ctx, int64_t gesture);
/** As setValue; calls with one `gesture`, from beginGesture, on one member fold into one undo
 * step. */
Status setValueGesture(Context &ctx, const string &path, const PropValue &value, int64_t gesture);
/** The raw write, not recorded: for a command's exec and undo, on state they capture
 * themselves. Don't set one member through both setValue and writeValue in a command. */
Status writeValue(Context &ctx, const string &path, const PropValue &value);
/** Runs an invocation, `mesh.extrude(distance=2)`, returning its outputs. From a script
 * command's exec it runs as a sub-command. */
BagResult invoke(Context &ctx, const string &invocation);
Status undo(Context &ctx);
Status redo(Context &ctx);
/** Whether undo or redo would do something. FailedPrecondition without an executor. */
BoolResult canUndo(Context &ctx);
BoolResult canRedo(Context &ctx);

} // namespace coredcc::script
