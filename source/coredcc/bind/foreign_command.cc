#include "coredcc/bind/foreign_command.h"

#include "coredcc/bind/detail.h"

#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

#include "coredcc/base/text.h"

#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace coredcc::script {

namespace lb = litestl::binding;
namespace lf = litestl::binding::foreign;

using detail::error;
using detail::scriptProp;
using detail::splitSlots;
using detail::view;

namespace {

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
    return fb_.call(handleOf(self), table.ifaceDesc, table.ifaceDesc->methods[slot], args, ret);
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
  /* Records would call through freed tables and this dispatcher: a silent UAF. */
  CDCC_CHECK(live_ == 0,
             "script command instances outlive the Runtime: destroy the executor and the "
             "history before it");
  for (lf::Handle h : handles_) {
    lf::release(h);
  }
  litestl::alloc::Delete(dispatcher_);
}

ForeignDispatcher *ForeignBindings::dispatcher()
{
  return dispatcher_;
}

Status ForeignBindings::call(lf::Handle h,
                             const lbt::_StructBase *iface,
                             const lbt::Method *m,
                             void **args,
                             void *ret)
{
  string err;
  frames_.append(Frame{iface, m});
  const bool ok = lf::call(h, m, args, ret, &err);
  frames_.pop_back();
  if (ok) {
    return Status();
  }
  return Status::error(StatusCode::Internal,
                       err.size() > 0 ? err.c_str() : "a script call failed");
}

void ForeignBindings::keepHandle(lf::Handle h)
{
  handles_.append(h);
}

bool inForeignCall(const Runtime &rt)
{
  const ForeignBindings *fb = rt.findExt<ForeignBindings>();
  return fb && fb->depth() > 0;
}

bool inForeignSlot(const Runtime &rt,
                   const lbt::_StructBase *iface,
                   std::initializer_list<const char *> slots)
{
  const ForeignBindings *fb = rt.findExt<ForeignBindings>();
  const ForeignBindings::Frame *f = fb ? fb->currentFrame() : nullptr;
  /* By name: another module may have a descriptor of its own. */
  if (!f || strcmp(f->iface->name.c_str(), iface->name.c_str()) != 0) {
    return false;
  }
  for (const char *s : slots) {
    if (strcmp(f->method->name.c_str(), s) == 0) {
      return true;
    }
  }
  return slots.size() == 0;
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

const lb::BindingBase *unwrap(const lb::BindingBase *t)
{
  while (t && t->type == lb::BindingType::ParentTemplParam) {
    t = static_cast<const lbt::ParentTemplateParam *>(t)->concreteType;
  }
  return t;
}

/* Why a script can't return `m`'s type; null if it can. These are the rules of litestl's
 * foreign dispatch (writeReturn), which would otherwise fail on the first call. */
const char *cantReturn(const lbt::Method &m)
{
  const lb::BindingBase *t = unwrap(m.returnType);
  if (!t) {
    return nullptr;
  }
  if (t->type == lb::BindingType::Reference) {
    return "returns a reference";
  }
  if (t->type == lb::BindingType::Pointer) {
    return m.returnPolicy == lbt::ReturnPolicy::Owned ? "returns ownership of a pointer" : nullptr;
  }
  if (t->type != lb::BindingType::Struct) {
    return nullptr;
  }
  const auto *st = static_cast<const lbt::_StructBase *>(t);
  const std::string_view name = st->name.c_str();
  if (name == "litestl::util::String") {
    return nullptr;
  }
  if (name == "litestl::util::Vector") {
    const lb::BindingBase *e = unwrap(st->templateParams[0].type);
    const bool scalar = e->type == lb::BindingType::Number ||
                        e->type == lb::BindingType::Boolean || e->type == lb::BindingType::Enum;
    return scalar ? nullptr : "returns a Vector whose elements aren't scalars";
  }
  bool hasDefault = false, hasCopy = false;
  for (const lbt::Constructor *c : st->constructors) {
    hasDefault = hasDefault || c->params.size() == 0;
    if (c->params.size() == 1 && c->params[0].type->type == lb::BindingType::Reference) {
      const auto *ref = static_cast<const lbt::Reference *>(c->params[0].type);
      hasCopy = hasCopy || ref->refType->buildFullName() == st->buildFullName();
    }
  }
  return hasDefault && hasCopy ? nullptr :
                                 "returns a struct without bound default and copy constructors";
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
    const char *why = cant ? "takes or returns a void * or a QueryAnswer" : cantReturn(*m);
    if (why) {
      bad += std::string(bad.empty() ? "" : "; ") + m->name.c_str() + " " + why;
    }
  }
  if (bad.empty()) {
    return Status();
  }
  return error(StatusCode::Unsupported,
               std::string(iface->name.c_str()) + ": scripts can't implement it: " + bad);
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

Status defDescription(CommandDef &def, const string &s)
{
  def.description(view(s));
  return Status();
}

Status defIcon(CommandDef &def, const string &s)
{
  def.icon(view(s));
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

Result<PropertyDef> detail::scriptProp(const char *what,
                                       const string &name,
                                       const PropValue &defaultValue,
                                       bool taken)
{
  const std::string_view n = view(name);
  if (!isIdentifier(n)) {
    return error(StatusCode::InvalidArgument,
                 std::string(what) + " '" + name.c_str() + "': not an identifier");
  }
  if (taken) {
    return error(StatusCode::AlreadyExists,
                 std::string(what) + " '" + name.c_str() + "': the def already has one");
  }
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

Status defInput(CommandDef &def, const string &name, const PropValue &defaultValue)
{
  const bool taken = def.findInput(view(name)) != nullptr;
  Result<PropertyDef> pd = scriptProp("defInput", name, defaultValue, taken);
  if (!pd.ok()) {
    return pd.status();
  }
  def.input(std::move(pd).value());
  return Status();
}

Status defOutput(CommandDef &def, const string &name, const PropValue &defaultValue)
{
  bool taken = false;
  for (int i = 0; i < def.outputCount(); i++) {
    taken = taken || def.outputDef(i).apiName() == name;
  }
  Result<PropertyDef> pd = scriptProp("defOutput", name, defaultValue, taken);
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
  Status s = rt.ext<ForeignBindings>().call(
      factory, ForeignFactory::defineBindings(), make, nullptr, &h);
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
  ForeignBindings &fb = rt.ext<ForeignBindings>();
  fb.instanceMade();
  return Owned<Command>(
      c,
      [](void *self, void *ctx) {
        static_cast<ForeignBindings *>(ctx)->instanceGone();
        lf::release(handleOf(self));
      },
      &fb);
}

} // namespace

std::vector<std::string> detail::splitSlots(const string &slots)
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

namespace {

Status registerWith(Runtime &rt, const CommandDef &def, lf::Handle factory, const string &slots)
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
  /* A script taking core.setProperty would break setValue for the whole Runtime. */
  if (cmd.rfind("core.", 0) == 0) {
    return error(StatusCode::InvalidArgument,
                 "command '" + cmd + "': the 'core.' namespace is reserved for coredcc");
  }
  /* The native fold protocol downcasts `next`, which a script command can't do. */
  if (has("foldKey") || has("foldFrom")) {
    return error(StatusCode::Unsupported,
                 "command '" + cmd + "': scripts can't fold commands (foldKey, foldFrom) yet");
  }
  CDCC_TRY(foreignImplementable(coredcc::detail::ifaceDesc<Command>()));

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
  /* A copy: the script's builder stays usable, and registering it twice is a clear
   * AlreadyExists. */
  CommandDef copy = def;
  copy.impl(impl, [type, factory](Runtime &r) { return makeInstance(r, type, factory); });
  Result<const CommandDef *> added = reg.add(std::move(copy));
  return added.ok() ? Status() : added.status();
}

} // namespace

Status registerForeignCommand(Context &ctx,
                              const CommandDef &def,
                              uint32_t factory,
                              const string &slots)
{
  if (factory == 0) {
    return error(StatusCode::InvalidArgument, "registerForeignCommand: no factory handle");
  }
  Runtime &rt = ctx.runtime();
  Status s = registerWith(rt, def, factory, slots);
  if (s.ok()) {
    rt.ext<ForeignBindings>().keepHandle(factory);
  }
  else {
    lf::release(factory);
  }
  return s;
}

} // namespace coredcc::script
