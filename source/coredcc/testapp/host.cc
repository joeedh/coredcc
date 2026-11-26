/* The test app's WASM module (docs/plans/2026-11-20-p1-application-spine.md, "P1f
 * step 6"): one host object owning a Runtime, a Library, a small root struct, an
 * executor and a LinearHistory, bound with coredcc's bindings. The NW.js test app
 * (apps/testapp) and the TS spike (tests/wasm/spike.test.ts) both drive it. */
#include "coredcc/api/change_bus.h"
#include "coredcc/api/struct.h"
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/graph_facades.h"
#include "coredcc/bind/manager.h"
#include "coredcc/bind/path_reader.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/last_used.h"
#include "coredcc/drive/drive.h"
#include "coredcc/drive/driver_list.h"
#include "coredcc/graph/commands.h"
#include "coredcc/graph/provider.h"
#include "coredcc/lib/library.h"
#include "coredcc/scene/commands.h"
#include "coredcc/testapp/boxes.h"
#include "coredcc/testapp/items.h"
#include "coredcc/tool/builtins.h"
#include "coredcc/tool/manager.h"
#include "coredcc/tool/settings.h"

#include "litestl/binding/binding.h"
#include "litestl/binding/manager.h"
#include "litestl/util/alloc.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <functional>

namespace testapp {

using namespace coredcc;
namespace lbt = litestl::binding::types;

/* The context root: `count`, `strength`, and one member of each other value kind, with
 * the items (items.h). */
struct Data {
  int count = 0;
  double strength = 0.5;
  bool enabled = true;
  litestl::util::string label;
  double offset[3] = {0, 0, 0};
  int mode = 0;
  uint32_t flags = 0;
  int64_t big = 0;
  /** `level` goes to `count`; `focus` is one of the items. */
  double level = 0;
  int64_t focus = 0;
  RefCell active{RefKind::Weak};
};

inline const StructId kData = StructId::of("TestApp");

/* The graphs, by name. */
CollectionAccessor graphsAccessor()
{
  const StructId type = StructId::of(kGraphDefType);
  auto find = [type](Context &ctx, const std::function<bool(const Datablock &)> &want) {
    PointerRef r;
    const Library &lib = *ctx.library();
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (!r.data && graphDefOf(lib, h) && want(b)) {
        r.data = graphDefOf(lib, h);
        r.type = type;
        r.owner = OwnerKey::ofBlock(b.id());
      }
    });
    return r;
  };
  CollectionAccessor c;
  c.length = [](const PointerRef &, Context &ctx) {
    int64_t n = 0;
    ctx.library()->forEach([&](BlockHandle h, const Datablock &) {
      n += graphDefOf(*ctx.library(), h) != nullptr;
    });
    return n;
  };
  c.at = [find](const PointerRef &, Context &ctx, int64_t i) {
    return find(ctx, [i](const Datablock &) mutable { return i-- == 0; });
  };
  c.lookup = [find](const PointerRef &, Context &ctx, const KeySegment &key) {
    return find(ctx, [&key](const Datablock &b) { return b.name() == key.text; });
  };
  c.keyOf = [](const PointerRef &, Context &ctx, const PointerRef &item) {
    const Library &lib = *ctx.library();
    return KeySegment::stringKey(lib.get(lib.find(item.owner.block))->name().c_str());
  };
  c.itemStruct = type;
  return c;
}

/**
 * Everything one document needs, owned in destruction order: the context, executor and
 * history go before the Library and the Runtime (bind/foreign_command.h, "Teardown").
 * Scripts get one of these instead of separate objects, whose GC order is undefined.
 */
class Host {
public:
  explicit Host(bool items) : lib_(&rt_), ctx_(makeContext(items))
  {
    ctx_.setExecutor(&ex_);
    ctx_.setTools(&tools_);
  }
  ~Host()
  {
    tools_.shutdown(ctx_);
    ctx_.setTools(nullptr);
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
  /** A change to `strength` in C++, as a simulation would make. */
  void nudge()
  {
    data_.strength += 0.125;
    rt_.ext<ChangeBus>().notify(ctx_.root().owner, "strength");
  }
  CommandExecutor &executor()
  {
    return ex_;
  }

  static const lbt::Struct<Host> *defineBindings()
  {
    static lbt::Struct<Host> *st = nullptr;
    if (!st) {
      litestl::alloc::PermanentGuard guard;
      st = new lbt::Struct<Host>("testapp::Host", sizeof(Host));
    }
    return st;
  }

private:
  Context makeContext(bool items)
  {
    registerItems(rt_);
    if (items) {
      addItems(lib_);
    }
    ApiRegistry &reg = rt_.ext<ApiRegistry>();
    ApiStruct s("TestApp");
    s.addField(PropertyDef::intProp("count"), &Data::count);
    s.addField(PropertyDef::floatProp("strength")
                   .range(0, 1)
                   .icon("bolt")
                   .description("How strong"),
               &Data::strength);
    s.addField(PropertyDef::boolProp("enabled"), &Data::enabled);
    s.addField(PropertyDef::stringProp("label"), &Data::label);
    s.addField(PropertyDef::vecProp("offset", 3), &Data::offset);
    s.addField(PropertyDef::enumProp("mode").item("OBJECT", 0).item("EDIT", 1),
               &Data::mode);
    s.addField(PropertyDef::flagsProp("flags")
                   .item("SELECT", 1, "Select", "Can be selected", "cursor")
                   .item("HIDE", 2)
                   .ui(PropUi{true, false, SliderStyle::Default, false}),
               &Data::flags);
    CDCC_CHECK(registerGraphCommands(rt_).ok(), "testapp: registering graphs");
    s.addCollection(PropertyDef::collectionProp("graphs", StructId::of(kGraphDefType)),
                    graphsAccessor());
    s.addField(PropertyDef::intProp("big"), &Data::big);
    addItemMembers(s, data_.active);
    s.addField(PropertyDef::floatProp("level").range(0, 1).dynamic(
                   [](PropertyDef &d, const void *owner) {
                     d.range(0, std::max(1, static_cast<const Data *>(owner)->count));
                   }),
               &Data::level);
    s.addField(PropertyDef::enumProp("focus").item("NONE", 0).ui(
                   PropUi{false, true, SliderStyle::Default, false}),
               &Data::focus)
        .dynamic([](PropertyDef &d, const PointerRef &, Context &ctx) {
          addItemKeys(d, ctx);
        });
    rt_.ext<LastUsedStore>().addCollectionTo(s);
    rt_.ext<ToolSettingsStore>().addCollectionTo(s);
    CDCC_CHECK(registerSceneCommands(rt_).ok(), "testapp: registering scenes");
    CDCC_CHECK(registerDriveTypes(rt_).ok(), "testapp: registering drivers");
    addDriverGraphsMember(s);
    CDCC_CHECK(registerToolBuiltins(rt_).ok(), "testapp: registering tools");
    registerBoxes(rt_);
    PointerAccessor scene;
    scene.target = StructId::of(kSceneType);
    scene.get = [](const PointerRef &, Context &ctx) {
      return coredcc::detail::blockRoot(ctx, ctx.scene());
    };
    s.addPointer(PropertyDef::pointerProp("scene", scene.target), std::move(scene));
    addEvaluated(s);
    CDCC_CHECK(reg.add(std::move(s)).ok(), "testapp: registering the root struct");
    Data *data = &data_;
    CDCC_CHECK(
        reg.addRuntimeOwner("testapp", kData, [data]() { return (void *)data; }).ok(),
        "testapp: registering the root owner");
    return Context(&rt_, &lib_, reg.runtimeOwner("testapp"));
  }

  Runtime rt_;
  Library lib_;
  Data data_;
  LinearHistory history_;
  CommandExecutor ex_{&rt_, &history_};
  Context ctx_;
  ToolManager tools_{rt_};
};

Host *create()
{
  return litestl::alloc::New<Host>("testapp::Host", true);
}
/** A document without blocks, to load into. */
Host *createEmpty()
{
  return litestl::alloc::New<Host>("testapp::Host", false);
}
Context *context(Host &h)
{
  return &h.context();
}
void nudge(Host &h)
{
  h.nudge();
}
int32_t historySize(Host &h)
{
  return h.history().size();
}
int32_t watched(Host &h)
{
  return script::watchedCount(h.context());
}
int32_t graphInstances(Host &h)
{
  return script::graphInstanceCount(h.context());
}
int32_t liveInstances(Host &h)
{
  const script::ForeignBindings *fb = h.runtime().findExt<script::ForeignBindings>();
  return fb ? fb->liveInstances() : 0;
}
/** What is allocated, by everything: for tests of what is given back. */
int32_t liveBytes()
{
  return int32_t(litestl::alloc::getMemorySize());
}
/** The Runtime's and the history's invariants, and that no command is still running:
 * called between commands, a running executor means one was abandoned mid-run. */
Status validate(Host &h)
{
  if (h.executor().running()) {
    return Status::error(StatusCode::Internal, "the executor is still running a command");
  }
  CDCC_TRY(h.runtime().validateAll());
  return h.history().validate();
}

/** A new box data block (TA3), as pointer text for core.object.add's `data`. */
litestl::util::string addBox(Host &h, double half)
{
  return litestl::util::string(createBox(h.context(), half).c_str());
}

/** Aborts the module, for tests of how the page reports a dead backend. */
void abortForTest()
{
  CDCC_CHECK(false, "testapp::abortForTest");
}

litestl::binding::BindingManager *manager()
{
  static litestl::binding::BindingManager *mgr = nullptr;
  if (!mgr) {
    litestl::alloc::PermanentGuard guard;
    mgr = litestl::alloc::New<litestl::binding::BindingManager>("testapp BindingManager");
    script::addBindings(mgr);
    mgr->add(litestl::binding::Bind<Host>());
    BIND_FUNCTION(mgr, "testapp::create", testapp::create, MARGS()).returnsOwned();
    BIND_FUNCTION(mgr, "testapp::createEmpty", testapp::createEmpty, MARGS())
        .returnsOwned();
    BIND_FUNCTION(mgr, "testapp::nudge", testapp::nudge, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::context", testapp::context, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::historySize", testapp::historySize, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::watched", testapp::watched, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::graphInstances", testapp::graphInstances, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::liveInstances", testapp::liveInstances, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::liveBytes", testapp::liveBytes, MARGS());
    BIND_FUNCTION(mgr, "testapp::validate", testapp::validate, MARGS("h"));
    BIND_FUNCTION(mgr, "testapp::abortForTest", testapp::abortForTest, MARGS());
    BIND_FUNCTION(mgr, "testapp::addBox", testapp::addBox, MARGS("h", "half"));
    mgr->validate();
  }
  return mgr;
}

} // namespace testapp

extern "C" EMSCRIPTEN_KEEPALIVE litestl::binding::BindingManager *TESTAPP_GetManager()
{
  return testapp::manager();
}
