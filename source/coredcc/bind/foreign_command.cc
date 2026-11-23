#include "coredcc/bind/foreign_command.h"

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coredcc::script {

namespace lb = litestl::binding;
namespace lf = litestl::binding::foreign;

namespace {

std::string_view view(const string &s)
{
  return std::string_view(s.c_str(), size_t(s.size()));
}

Status error(StatusCode code, const std::string &message)
{
  return Status::error(code, message.c_str());
}

void *selfOf(lf::Handle h)
{
  return reinterpret_cast<void *>(uintptr_t(h));
}

lf::Handle handleOf(void *self)
{
  return lf::Handle(reinterpret_cast<uintptr_t>(self));
}

} // namespace

/* ---- ForeignFactory ---- */

const lbt::Struct<ForeignFactory> *ForeignFactory::defineBindings()
{
  static lbt::Struct<ForeignFactory> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ForeignFactory>("coredcc::ForeignFactory", sizeof(ForeignFactory));
    st->isInterface = true;
    BIND_STRUCT_METHOD(st, make, MARGS());
  }
  return st;
}

/* ---- ForeignBindings ---- */

/** Every foreign table's dispatcher: `self` is the instance's handle. */
class ForeignBindings::Dispatcher final : public ForeignDispatcher {
 public:
  explicit Dispatcher(ForeignBindings *fb) : fb_(*fb) {}
  Status call(const IfaceTable &table, void *self, int slot, void **args, void *ret) override
  {
    return fb_.call(handleOf(self), table.ifaceDesc->methods[slot], args, ret);
  }

 private:
  ForeignBindings &fb_;
};

ForeignBindings::ForeignBindings(Runtime *)
    : dispatcher_(litestl::alloc::New<Dispatcher>("coredcc::script::Dispatcher", this))
{
}

ForeignBindings::~ForeignBindings()
{
  for (lf::Handle h : factories_) {
    lf::release(h);
  }
  litestl::alloc::Delete(dispatcher_);
}

ForeignDispatcher *ForeignBindings::dispatcher()
{
  return dispatcher_;
}

Status ForeignBindings::call(lf::Handle h, const lbt::Method *m, void **args, void *ret)
{
  string err;
  depth_++;
  const bool ok = lf::call(h, m, args, ret, &err);
  depth_--;
  if (ok) {
    return Status();
  }
  return Status::error(StatusCode::Internal,
                       err.size() > 0 ? err.c_str() : "a script call failed");
}

void ForeignBindings::keepFactory(lf::Handle h)
{
  factories_.append(h);
}

bool inForeignCall(const Runtime &rt)
{
  const ForeignBindings *fb = rt.findExt<ForeignBindings>();
  return fb && fb->depth() > 0;
}

/* ---- foreignImplementable ---- */

namespace {

/* A `void *` (bound as the Number "pointer") or a QueryAnswer, under any wrapping. */
bool scriptCantCarry(const lb::BindingBase *t)
{
  while (t) {
    switch (t->type) {
      case lb::BindingType::ParentTemplParam:
        t = static_cast<const lbt::ParentTemplateParam *>(t)->concreteType;
        break;
      case lb::BindingType::Reference:
        t = static_cast<const lbt::Reference *>(t)->refType;
        break;
      case lb::BindingType::Pointer:
        t = static_cast<const lbt::Pointer *>(t)->ptrType;
        break;
      case lb::BindingType::Number:
        return strcmp(t->name.c_str(), "pointer") == 0;
      case lb::BindingType::Struct:
        return strcmp(t->name.c_str(), "coredcc::QueryAnswer") == 0;
      default:
        return false;
    }
  }
  return false;
}

} // namespace

Status foreignImplementable(const lbt::_StructBase *iface)
{
  std::string bad;
  for (const lbt::Method *m : iface->methods) {
    bool cant = scriptCantCarry(m->returnType);
    for (const lbt::MethodParam &p : m->params) {
      cant = cant || scriptCantCarry(p.type);
    }
    if (cant) {
      bad += std::string(bad.empty() ? "" : ", ") + m->name.c_str();
    }
  }
  if (bad.empty()) {
    return Status();
  }
  return error(StatusCode::Unsupported,
               std::string(iface->name.c_str()) +
                   ": scripts can't implement slots taking or returning a void * or a "
                   "QueryAnswer: " +
                   bad);
}

/* ---- CommandDef builder facades ---- */

CommandDef *newCommandDef(const string &id)
{
  return litestl::alloc::New<CommandDef>("coredcc::CommandDef", view(id));
}

Status defUiName(CommandDef &def, const string &s)
{
  def.uiName(view(s));
  return Status();
}

Status defFlags(CommandDef &def, uint32_t flags)
{
  const uint32_t known = uint32_t(CommandFlags::Private | CommandFlags::NoUndo |
                                  CommandFlags::UndoBarrier | CommandFlags::Interactive);
  if ((flags & ~known) != 0) {
    return error(StatusCode::InvalidArgument,
                 "defFlags: unknown flag bits " + std::to_string(flags & ~known));
  }
  def.flags(CommandFlags(flags));
  return Status();
}

namespace {

/* A def of `defaultValue`'s kind, for defInput and defOutput. */
Result<PropertyDef> scriptProp(const char *what, const string &name, const PropValue &defaultValue)
{
  const std::string_view n = view(name);
  PropertyDef pd = PropertyDef::boolProp(n);
  switch (defaultValue.type()) {
    case PropValue::Type::Bool:
      break;
    case PropValue::Type::Int:
      pd = PropertyDef::intProp(n);
      break;
    case PropValue::Type::Float:
      pd = PropertyDef::floatProp(n);
      break;
    case PropValue::Type::String:
      pd = PropertyDef::stringProp(n);
      break;
    case PropValue::Type::Vec2:
    case PropValue::Type::Vec3:
    case PropValue::Type::Vec4:
      pd = PropertyDef::vecProp(n, defaultValue.componentCount());
      break;
    default:
      return error(StatusCode::InvalidArgument,
                   std::string(what) + " '" + name.c_str() + "': a script property can't be " +
                       propValueTypeName(defaultValue.type()) +
                       " (Bool, Int, Float, String and Vec2-4 can)");
  }
  pd.defaultValue(defaultValue);
  return pd;
}

} // namespace

Status defInput(CommandDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> pd = scriptProp("defInput", name, defaultValue);
  if (!pd.ok()) {
    return pd.status();
  }
  def.input(std::move(pd).value());
  return Status();
}

Status defOutput(CommandDef &def, const string &name, const PropValue &defaultValue)
{
  Result<PropertyDef> pd = scriptProp("defOutput", name, defaultValue);
  if (!pd.ok()) {
    return pd.status();
  }
  def.output(std::move(pd).value());
  return Status();
}

/* ---- registration ---- */

namespace {

Result<Owned<Command>> makeInstance(Runtime &rt, TypeId type, lf::Handle factory)
{
  const lbt::Method *make = ForeignFactory::defineBindings()->findMethod("make");
  lf::Handle h = 0;
  Status s = rt.ext<ForeignBindings>().call(factory, make, nullptr, &h);
  if (!s.ok()) {
    return error(s.code(), std::string("the command factory failed: ") + s.message());
  }
  if (h == 0) {
    return error(StatusCode::Internal, "the command factory's make returned no instance");
  }
  Command c = query<Command>(rt, type, selfOf(h));
  if (!c) {
    lf::release(h);
    return error(StatusCode::NotFound, "the script command's table is gone");
  }
  return Owned<Command>(c, [](void *self, void *) { lf::release(handleOf(self)); });
}

std::vector<std::string> splitSlots(const string &slots)
{
  std::vector<std::string> out;
  std::string cur;
  for (const char *p = slots.c_str(); *p; p++) {
    if (*p == ' ') {
      if (!cur.empty()) {
        out.push_back(std::move(cur));
        cur.clear();
      }
    }
    else {
      cur += *p;
    }
  }
  if (!cur.empty()) {
    out.push_back(std::move(cur));
  }
  return out;
}

Status registerWith(Runtime &rt, CommandDef &def, lf::Handle factory, const string &slots)
{
  const std::vector<std::string> names = splitSlots(slots);
  auto has = [&names](const char *slot) {
    for (const std::string &n : names) {
      if (n == slot) {
        return true;
      }
    }
    return false;
  };
  CommandRegistry &reg = rt.ext<CommandRegistry>();
  CDCC_TRY(reg.checkDef(def));
  CDCC_TRY(CommandRegistry::checkSlots(def, has("redo"), has("foldKey"), has("foldFrom")));
  const std::string cmd = def.name().c_str();
  /* The native fold protocol downcasts `next`, which a script command can't do. */
  if (has("foldKey") || has("foldFrom")) {
    return error(StatusCode::Unsupported,
                 "command '" + cmd + "': scripts can't fold commands (foldKey, foldFrom) yet");
  }
  CDCC_TRY(foreignImplementable(detail::ifaceDesc<Command>()));

  std::vector<const char *> present;
  for (const std::string &n : names) {
    present.push_back(n.c_str());
  }
  const std::string impl = cmd + "#impl";
  CDCC_TRY(registerForeign<Command>(rt,
                                    impl,
                                    rt.ext<ForeignBindings>().dispatcher(),
                                    nullptr,
                                    std::span<const char *const>(present.data(), present.size())));
  const TypeId type = TypeId::of(impl);
  def.impl(impl, [type, factory](Runtime &r) { return makeInstance(r, type, factory); });
  Result<const CommandDef *> added = reg.add(std::move(def));
  return added.ok() ? Status() : added.status();
}

} // namespace

Status registerForeignCommand(Context &ctx,
                              CommandDef &def,
                              uint32_t factory,
                              const string &slots)
{
  if (factory == 0) {
    return error(StatusCode::InvalidArgument, "registerForeignCommand: no factory handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, def, factory, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepFactory(factory);
  }
  else {
    lf::release(factory);
  }
  return s;
}

} // namespace coredcc::script
