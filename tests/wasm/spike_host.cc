/* The P1f spike's WASM module: one host object owning a Runtime, a Library, a small
 * root struct, an executor and a LinearHistory, bound with coredcc's bindings.
 * spike.test.ts drives it through the TS runtime and coredcc's TS layer. */
#include "coredcc/api/struct.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/manager.h"
#include "coredcc/command/executor.h"
#include "coredcc/lib/library.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/manager.h"
#include "litestl/util/alloc.h"

#include <emscripten/emscripten.h>

namespace spike {

using namespace coredcc;
namespace lbt = litestl::binding::types;

/* The context root: `count`, `strength`, and one member of each other value kind. */
struct Data {
  int count = 0;
  double strength = 0.5;
  bool enabled = true;
  litestl::util::string label;
  double offset[3] = {0, 0, 0};
  int mode = 0;
  uint32_t flags = 0;
};

inline const StructId kData = StructId::of("Spike");

/**
 * Everything one document needs, owned in destruction order: the context, executor and
 * history go before the Library and the Runtime (bind/foreign_command.h, "Teardown").
 * Scripts get one of these instead of separate objects, whose GC order is undefined.
 */
class Host {
 public:
  Host() : lib_(&rt_), ctx_(makeContext())
  {
    ctx_.setExecutor(&ex_);
  }
  ~Host()
  {
    history_.clear();
    rt_.takeStickyError().ignore(); /* a script discard that threw during clear() */
  }
  Host(const Host &) = delete;
  Host &operator=(const Host &) = delete;

  Context &context()
  {
    return ctx_;
  }
  Runtime &runtime()
  {
    return rt_;
  }
  LinearHistory &history()
  {
    return history_;
  }

  static const lbt::Struct<Host> *defineBindings()
  {
    static lbt::Struct<Host> *st = nullptr;
    if (!st) {
      litestl::alloc::PermanentGuard guard;
      st = new lbt::Struct<Host>("spike::Host", sizeof(Host));
    }
    return st;
  }

 private:
  Context makeContext()
  {
    ApiRegistry &reg = rt_.ext<ApiRegistry>();
    ApiStruct s("Spike");
    s.addField(PropertyDef::intProp("count"), &Data::count);
    s.addField(PropertyDef::floatProp("strength").range(0, 1), &Data::strength);
    s.addField(PropertyDef::boolProp("enabled"), &Data::enabled);
    s.addField(PropertyDef::stringProp("label"), &Data::label);
    s.addField(PropertyDef::vecProp("offset", 3), &Data::offset);
    s.addField(PropertyDef::enumProp("mode").item("OBJECT", 0).item("EDIT", 1), &Data::mode);
    s.addField(PropertyDef::flagsProp("flags").item("SELECT", 1).item("HIDE", 2), &Data::flags);
    CDCC_CHECK(reg.add(std::move(s)).ok(), "spike: registering the root struct");
    Data *data = &data_;
    CDCC_CHECK(reg.addRuntimeOwner("spike", kData, [data]() { return (void *)data; }).ok(),
               "spike: registering the root owner");
    return Context(&rt_, &lib_, reg.runtimeOwner("spike"));
  }

  Runtime rt_;
  Library lib_;
  Data data_;
  LinearHistory history_;
  CommandExecutor ex_{&rt_, &history_};
  Context ctx_;
};

Host *create()
{
  return litestl::alloc::New<Host>("spike::Host");
}
Context *context(Host &h)
{
  return &h.context();
}
int32_t historySize(Host &h)
{
  return h.history().size();
}
int32_t liveInstances(Host &h)
{
  const script::ForeignBindings *fb = h.runtime().findExt<script::ForeignBindings>();
  return fb ? fb->liveInstances() : 0;
}
/** The Runtime's and the history's invariants. */
Status validate(Host &h)
{
  CDCC_TRY(h.runtime().validateAll());
  return h.history().validate();
}

litestl::binding::BindingManager *manager()
{
  static litestl::binding::BindingManager *mgr = nullptr;
  if (!mgr) {
    litestl::alloc::PermanentGuard guard;
    mgr = litestl::alloc::New<litestl::binding::BindingManager>("spike BindingManager");
    script::addBindings(mgr);
    mgr->add(litestl::binding::Bind<Host>());
    BIND_FUNCTION(mgr, "spike::create", spike::create, MARGS()).returnsOwned();
    BIND_FUNCTION(mgr, "spike::context", spike::context, MARGS("h"));
    BIND_FUNCTION(mgr, "spike::historySize", spike::historySize, MARGS("h"));
    BIND_FUNCTION(mgr, "spike::liveInstances", spike::liveInstances, MARGS("h"));
    BIND_FUNCTION(mgr, "spike::validate", spike::validate, MARGS("h"));
    mgr->validate();
  }
  return mgr;
}

} // namespace spike

extern "C" EMSCRIPTEN_KEEPALIVE litestl::binding::BindingManager *SPIKE_GetManager()
{
  return spike::manager();
}
