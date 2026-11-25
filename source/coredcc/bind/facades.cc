#include "coredcc/bind/facades.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/api/resolve.h"
#include "coredcc/base/text.h"
#include "coredcc/bind/detail.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/meta.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/set_property.h"
#include "coredcc/graph/node_type.h"
#include "coredcc/parse/invocation.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <string>
#include <string_view>

namespace coredcc::script {

namespace lbt = litestl::binding::types;
using detail::error;
using detail::failed;
using detail::ok;
using detail::slotGate;
using detail::view;

namespace {

Status wrongType(const PropValue &v, const char *want)
{
  return error(StatusCode::InvalidArgument,
               std::string("expected a ") + want + " value, not " +
                   propValueTypeName(v.type()));
}

Status notInSlot(const char *what)
{
  return error(StatusCode::FailedPrecondition,
               std::string(what) + ": only inside a command's slots");
}

ValueResult lookup(const PropertyBag &bag, const string &name)
{
  const PropValue *v = bag.get(view(name));
  if (v) {
    return ok<ValueResult>(*v);
  }
  Status err = error(StatusCode::NotFound, std::string("no property '") + name.c_str() + "'");
  Suggester sg(view(name));
  for (int i = 0; i < bag.size(); i++) {
    sg.consider(bag.def(i).apiName().c_str());
  }
  sg.addTo(err);
  return failed<ValueResult>(std::move(err));
}

} // namespace

/* ---- gates ---- */

Status detail::kernelGate(Context &ctx, const char *what)
{
  if (!inForeignSlot(ctx.runtime(), coredcc::detail::ifaceDesc<GraphKernel>())) {
    return Status();
  }
  return error(StatusCode::Busy, std::string(what) + ": not from a script kernel (only its "
                                                     "node's inputs, params and outputs)");
}

Status detail::slotGate(Context &ctx,
                        const char *what,
                        std::initializer_list<const char *> allowed)
{
  CDCC_TRY(kernelGate(ctx, what));
  const ForeignBindings *fb = ctx.runtime().findExt<ForeignBindings>();
  const ForeignBindings::Frame *frame = fb ? fb->currentFrame() : nullptr;
  if (!frame || (allowed.size() > 0 &&
                 inForeignSlot(ctx.runtime(), coredcc::detail::ifaceDesc<Command>(), allowed)))
  {
    return Status();
  }
  std::string where;
  for (const char *a : allowed) {
    where += std::string(where.empty() ? "" : ", ") + a;
  }
  return error(StatusCode::Busy,
               std::string(what) + ": not from a script's " + frame->iface->name.c_str() + "." +
                   frame->method->name.c_str() +
                   (where.empty() ? std::string(" (only outside script slots)")
                                  : " (only from a command's " + where + ")"));
}

Result<PropertyBag> detail::runCommand(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  CommandExecutor *ex = ctx.executor();
  return inForeignCall(ctx.runtime()) ? ex->runSub(ctx, def, std::move(inputs)) :
                                        ex->run(ctx, def, std::move(inputs));
}

/* ---- Status ---- */

Status statusError(StatusCode code, const string &message)
{
  const bool valid = code != StatusCode::Ok && uint32_t(code) <= uint32_t(StatusCode::Internal);
  return Status::error(valid ? code : StatusCode::InvalidArgument, message.c_str());
}

string statusText(const Status &s)
{
  return s.toString();
}

/* ---- PropValue ---- */

PropValue makeBool(bool v)
{
  return PropValue::ofBool(v);
}
PropValue makeInt(int64_t v)
{
  return PropValue::ofInt(v);
}
PropValue makeFloat(double v)
{
  return PropValue::ofFloat(v);
}
ValueResult makeString(const string &v)
{
  return ok<ValueResult>(PropValue::ofString(view(v)));
}
PropValue makeVec2(double x, double y)
{
  return PropValue::ofVec2(x, y);
}
PropValue makeVec3(double x, double y, double z)
{
  return PropValue::ofVec3(x, y, z);
}
PropValue makeVec4(double x, double y, double z, double w)
{
  return PropValue::ofVec4(x, y, z, w);
}
PropValue makeFlags(int64_t bits)
{
  return PropValue::ofFlags(uint64_t(bits));
}
PropValue makeNullPointer()
{
  return PropValue::nullPointer();
}
ValueResult makePointer(Context &ctx, const string &text)
{
  Result<StableRef> ref = parseRef(ctx.runtime(), view(text));
  return ref.ok() ? ok<ValueResult>(PropValue::ofPointer(std::move(ref).value())) :
                    failed<ValueResult>(ref.status());
}

BoolResult readBool(const PropValue &v)
{
  std::optional<bool> b = v.tryBool();
  return b ? ok<BoolResult>(*b) : failed<BoolResult>(wrongType(v, "Bool"));
}
IntResult readInt(const PropValue &v)
{
  std::optional<int64_t> i = v.tryInt();
  return i ? ok<IntResult>(*i) : failed<IntResult>(wrongType(v, "Int"));
}
FloatResult readFloat(const PropValue &v)
{
  std::optional<double> d = v.tryFloat();
  return d ? ok<FloatResult>(*d) : failed<FloatResult>(wrongType(v, "Float"));
}
StringResult readString(const PropValue &v)
{
  const string *s = v.tryString();
  return s ? ok<StringResult>(*s) : failed<StringResult>(wrongType(v, "String"));
}
FloatResult readComponent(const PropValue &v, int32_t i)
{
  const int n = v.componentCount();
  if (i < 0 || i >= n) {
    return failed<FloatResult>(error(StatusCode::OutOfRange,
                                     "component " + std::to_string(i) + " of a " +
                                         propValueTypeName(v.type()) + " value, which has " +
                                         std::to_string(n)));
  }
  return ok<FloatResult>(v.component(i));
}
IntResult readEnum(const PropValue &v)
{
  std::optional<int64_t> e = v.tryEnum();
  return e ? ok<IntResult>(*e) : failed<IntResult>(wrongType(v, "Enum"));
}
IntResult readFlags(const PropValue &v)
{
  std::optional<uint64_t> f = v.tryFlags();
  return f ? ok<IntResult>(int64_t(*f)) : failed<IntResult>(wrongType(v, "Flags"));
}
StringResult pointerText(Context &ctx, const PropValue &v)
{
  const StableRef *ref = v.tryPointer();
  return ref ? ok<StringResult>(string(formatRef(ctx.runtime(), *ref).c_str())) :
               failed<StringResult>(wrongType(v, "Pointer"));
}
string valueToString(const PropValue &v)
{
  return string(v.toString().c_str());
}

/* ---- PropertyBag ---- */

ValueResult bagGet(const PropertyBag &bag, const string &name)
{
  return lookup(bag, name);
}
int32_t bagSize(const PropertyBag &bag)
{
  return bag.size();
}
StringResult bagName(const PropertyBag &bag, int32_t i)
{
  if (i < 0 || i >= bag.size()) {
    return failed<StringResult>(error(StatusCode::OutOfRange,
                                      "entry " + std::to_string(i) + " of a bag of " +
                                          std::to_string(bag.size())));
  }
  return ok<StringResult>(bag.def(i).apiName());
}

/* ---- the running command's bags ---- */

ValueResult inputGet(Context &ctx, const string &name)
{
  if (Status s = detail::kernelGate(ctx, "inputGet"); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  if (!ctx.inputs()) {
    return failed<ValueResult>(notInSlot("inputGet"));
  }
  return lookup(*ctx.inputs(), name);
}
Status outputSet(Context &ctx, const string &name, const PropValue &value)
{
  CDCC_TRY(detail::kernelGate(ctx, "outputSet"));
  if (!ctx.outputs()) {
    return notInSlot("outputSet");
  }
  return ctx.outputs()->set(view(name), value);
}
ValueResult outputGet(Context &ctx, const string &name)
{
  if (Status s = detail::kernelGate(ctx, "outputGet"); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  if (!ctx.outputs()) {
    return failed<ValueResult>(notInSlot("outputGet"));
  }
  return lookup(*ctx.outputs(), name);
}

/* ---- datapaths and commands ---- */

StringResult resolvePath(Context &ctx, const string &path)
{
  if (Status s = detail::kernelGate(ctx, "resolvePath"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<ResolvedPath> r = resolve(ctx, view(path));
  return r.ok() ? ok<StringResult>(string(pathKindName(*r))) : failed<StringResult>(r.status());
}
StringResult pathMeta(Context &ctx, const string &path)
{
  if (Status s = detail::kernelGate(ctx, "pathMeta"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<std::string> r = coredcc::pathMeta(ctx, view(path));
  return r.ok() ? ok<StringResult>(string(r->c_str())) : failed<StringResult>(r.status());
}
StringResult valueKeys(Context &ctx, const string &path, const PropValue &v)
{
  const std::optional<int64_t> number = v.tryEnum();
  if (!number && !v.tryFlags()) {
    return failed<StringResult>(wrongType(v, "Enum or Flags"));
  }
  if (Status s = detail::kernelGate(ctx, "valueKeys"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<PropertyDef> def = getMeta(ctx, view(path));
  if (!def.ok()) {
    return failed<StringResult>(def.status());
  }
  if (!def->enumMeta()) {
    return failed<StringResult>(error(StatusCode::InvalidArgument,
                                      std::string("'") + path.c_str() + "' has no items"));
  }
  if (number) {
    const EnumItem *item = def->enumMeta()->findValue(*number);
    return item ? ok<StringResult>(item->key) :
                  failed<StringResult>(error(StatusCode::NotFound,
                                             "no item is " + std::to_string(*number)));
  }
  uint64_t bits = *v.tryFlags();
  std::string keys;
  for (const EnumItem &item : def->enumMeta()->items) {
    const uint64_t bit = uint64_t(item.value);
    if (bit != 0 && (bits & bit) == bit) {
      keys += std::string(keys.empty() ? "" : "|") + item.key.c_str();
      bits &= ~bit;
    }
  }
  return bits == 0 ? ok<StringResult>(string(keys.c_str())) :
                     failed<StringResult>(error(StatusCode::NotFound, "a flag bit has no item"));
}
ValueResult getValue(Context &ctx, const string &path)
{
  if (Status s = detail::kernelGate(ctx, "getValue"); !s.ok()) {
    return failed<ValueResult>(std::move(s));
  }
  Result<PropValue> r = coredcc::getValue(ctx, view(path));
  return r.ok() ? ok<ValueResult>(std::move(r).value()) : failed<ValueResult>(r.status());
}
static Status setUndo(Context &ctx,
                      const char *what,
                      const string &path,
                      const PropValue &value,
                      uint64_t gesture)
{
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) +
                     ": the context has no executor (writeValue writes unrecorded)");
  }
  /* From exec it becomes a sub-command; from any other slot it would push. */
  CDCC_TRY(slotGate(ctx, what, {"exec"}));
  return setValueUndo(ctx, view(path), value, gesture);
}
Status setValue(Context &ctx, const string &path, const PropValue &value)
{
  return setUndo(ctx, "setValue", path, value, 0);
}
Status setValueGesture(Context &ctx, const string &path, const PropValue &value, int64_t gesture)
{
  if (gesture <= 0) {
    return error(StatusCode::InvalidArgument, "setValueGesture: a gesture's id is positive");
  }
  return setUndo(ctx, "setValueGesture", path, value, uint64_t(gesture));
}
Status writeValue(Context &ctx, const string &path, const PropValue &value)
{
  /* Elsewhere (canRun, discard, make) no record would own the change. */
  CDCC_TRY(slotGate(ctx, "writeValue", {"exec", "undo", "redo"}));
  return coredcc::setValue(ctx, view(path), value);
}
BagResult invoke(Context &ctx, const string &invocation)
{
  if (Status s = slotGate(ctx, "invoke", {"exec"}); !s.ok()) {
    return failed<BagResult>(std::move(s));
  }
  CommandExecutor *ex = ctx.executor();
  if (!inForeignCall(ctx.runtime()) || !ex) {
    Result<PropertyBag> r = coredcc::invoke(ctx, view(invocation));
    return r.ok() ? ok<BagResult>(std::move(r).value()) : failed<BagResult>(r.status());
  }
  /* From a script command's exec: a sub-command, as setValue is. */
  Result<Invocation> inv = parseInvocation(view(invocation));
  if (!inv.ok()) {
    return failed<BagResult>(inv.status());
  }
  Runtime &rt = ctx.runtime();
  const CommandDef *def = rt.ext<CommandRegistry>().find(inv->command.c_str());
  if (!def) {
    return failed<BagResult>(rt.ext<CommandRegistry>().unknown(inv->command.c_str()));
  }
  Result<PropertyBag> inputs = inputsFromInvocation(ctx, *def, *inv);
  if (!inputs.ok()) {
    return failed<BagResult>(inputs.status());
  }
  Result<PropertyBag> r = detail::runCommand(ctx, *def, std::move(inputs).value());
  return r.ok() ? ok<BagResult>(std::move(r).value()) : failed<BagResult>(r.status());
}
Status undo(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "undo", {}));
  if (!ctx.executor()) {
    return error(StatusCode::FailedPrecondition, "undo: the context has no executor");
  }
  return ctx.executor()->undo(ctx);
}
Status redo(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "redo", {}));
  if (!ctx.executor()) {
    return error(StatusCode::FailedPrecondition, "redo: the context has no executor");
  }
  return ctx.executor()->redo(ctx);
}
static BoolResult historyCan(Context &ctx, const char *what, bool undo)
{
  if (Status s = detail::kernelGate(ctx, what); !s.ok()) {
    return failed<BoolResult>(std::move(s));
  }
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return failed<BoolResult>(
        error(StatusCode::FailedPrecondition, std::string(what) + ": the context has no executor"));
  }
  UndoHistory *h = ex->history();
  return ok<BoolResult>(h && (undo ? h->canUndo() : h->canRedo()));
}
BoolResult canUndo(Context &ctx)
{
  return historyCan(ctx, "canUndo", true);
}
BoolResult canRedo(Context &ctx)
{
  return historyCan(ctx, "canRedo", false);
}

/* ---- result struct descriptors ---- */

CDCC_RESULT_BINDINGS(BoolResult)
CDCC_RESULT_BINDINGS(IntResult)
CDCC_RESULT_BINDINGS(FloatResult)
CDCC_RESULT_BINDINGS(StringResult)
CDCC_RESULT_BINDINGS(ValueResult)
CDCC_RESULT_BINDINGS(BagResult)

} // namespace coredcc::script
