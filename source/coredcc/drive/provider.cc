/* SceneDrivers as the Library's OverrideProvider: evaluating the generated graph, and
 * answering from what it gave. */
#include "coredcc/api/change_bus.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/invalidate.h"
#include "coredcc/drive/state.h"
#include "coredcc/scene/transform_registry.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace coredcc {

using litestl::util::Vector;
using namespace drive;

namespace {

Status failed(const std::string &why)
{
  return Status::error(StatusCode::FailedPrecondition, why.c_str());
}

/* The flattened offset and kind of a channel def, by name; -1 if there is none. */
int32_t offsetOf(const TransformType &type, std::string_view name, DriveKind &kind)
{
  int32_t at = 0;
  for (const PropertyDef &def : type.channelDefs()) {
    const DriveKind k = kindOf(def.kind());
    if (name == def.apiName().c_str()) {
      kind = k;
      return at;
    }
    at += kindChannels(k);
  }
  return -1;
}

/* The Library's SceneDrivers, for SceneDrivers::of. */
struct AllDrivers {
  static constexpr const char *kExtName = "coredcc.drive.AllDrivers";
  explicit AllDrivers(Runtime *)
  {
  }
  struct Entry {
    LibraryRef lib;
    SceneDrivers *drivers = nullptr;
  };
  std::vector<Entry> entries;
};

bool sameStatus(const Status &a, const Status &b)
{
  return a.code() == b.code() && strcmp(a.message(), b.message()) == 0;
}

} // namespace

/* ---- evaluating ---- */

drive::EvalKey SceneDrivers::State::keyOf(const Library &lib, double time) const
{
  EvalKey k;
  k.lastEdit = rt->lastEdit();
  k.epoch = lib.epoch();
  k.clients = rt->ext<ClientRegistry>().generation();
  k.tables = rt->ifaces().tableCount();
  k.time = time;
  return k;
}

Status SceneDrivers::State::evaluate(Library &lib, double time)
{
  const Status was = callError;
  Vector<BlockId> moved;
  callError = inst->setInput("time", GraphValue::ofFloat(time));
  for (int attempt = 0; attempt < 2 && callError.ok(); attempt++) {
    Vector<int> changed;
    Status s = inst->evaluate(
        std::span<const SocketHandle>(handles.data(), handles.size()), changed);
    if (rt->hasStickyError()) {
      const Status sticky = rt->takeStickyError();
      s = s.ok() ? sticky : s;
    }
    if (!s.ok()) {
      callError = s;
      break;
    }
    bool stale = false;
    for (const SocketHandle &h : statusHandles) {
      const GraphValue *v = inst->value(h);
      stale = stale || (v && v->asInt() == kStaleStatus);
    }
    if (stale && attempt == 0) {
      callError = sync(lib, true);
      if (callError.ok()) {
        callError = inst->setInput("time", GraphValue::ofFloat(time));
      }
      continue;
    }
    callError = stale ? failed("an object's Transformable answers otherwise each time it "
                               "is read")
                      : Status();
    for (int i : changed) {
      if (i >= int(drives.size())) {
        continue;
      }
      DriveEntry &e = drives[i];
      const GraphValue *v = inst->value(e.handle);
      double values[4] = {};
      const bool ok = v && flatten(*v, e.kind, values);
      std::string error;
      if (!ok) {
        const GraphError *err = inst->error(e.handle);
        error = err ? err->status.message() : "its value isn't of its kind";
      }
      if (ok != e.ok || memcmp(values, e.values, sizeof(values)) != 0 || error != e.error)
      {
        moved.append(e.object);
      }
      e.ok = ok;
      memcpy(e.values, values, sizeof(values));
      e.error = error;
    }
    break;
  }
  /* The call as a whole stands for every driven object: all of them changed. */
  if (!sameStatus(was, callError)) {
    moved.clear();
    for (auto &pair : drivesOf) {
      moved.append(pair.key);
    }
  }
  /* Outside any evaluation: whoever evaluates a scene hears of it. */
  ChangeBus &bus = rt->ext<ChangeBus>();
  for (const BlockId &id : moved) {
    bus.notify(OwnerKey::ofBlock(id), {}, kDrivenAspect);
  }
  evaluated = true;
  evalKey = keyOf(lib, time);
  return callError;
}

Status SceneDrivers::State::ensure(Library &lib, double time)
{
  if (rt->evaluating()) {
    if (evaluated && keyOf(lib, time) == evalKey) {
      return callError;
    }
    return failed(
        "the drivers can't be evaluated while a scene or a graph evaluates, and "
        "weren't for this time");
  }
  CDCC_TRY(sync(lib, false));
  if (evaluated && keyOf(lib, time) == evalKey) {
    return callError;
  }
  return evaluate(lib, time);
}

/* ---- SceneDrivers ---- */

SceneDrivers::SceneDrivers(Library &lib) : s_(std::make_unique<State>())
{
  s_->rt = &lib.runtime();
  s_->lib = lib.ref();
}

Result<std::unique_ptr<SceneDrivers>> SceneDrivers::make(Library &lib)
{
  std::unique_ptr<SceneDrivers> d(new SceneDrivers(lib));
  CDCC_TRY(setOverrideProvider(lib, *d));
  State *s = d->s_.get();
  s->listener = lib.runtime().ext<ChangeBus>().addChangeListener([s](const BusChange &c) {
    if (!c.owner) {
      s->rescan = true;
    }
  });
  lib.runtime().ext<AllDrivers>().entries.push_back(
      AllDrivers::Entry{lib.ref(), d.get()});
  return d;
}

SceneDrivers *SceneDrivers::of(const Library &lib)
{
  for (const AllDrivers::Entry &e : lib.runtime().ext<AllDrivers>().entries) {
    if (e.lib.get() == &lib) {
      return e.drivers;
    }
  }
  return nullptr;
}

SceneDrivers::~SceneDrivers()
{
  std::erase_if(s_->rt->ext<AllDrivers>().entries, [this](const AllDrivers::Entry &e) {
    return e.drivers == this || !e.lib.get();
  });
  if (Library *lib = s_->lib.get()) {
    clearOverrideProvider(*lib, *this);
  }
  s_->inst.reset();
  s_->def.reset();
}

void SceneDrivers::setInner(OverrideProvider *inner)
{
  s_->inner = inner;
  s_->built = false; /* its kernels ask it */
  s_->evaluated = false;
  s_->rt->ext<ChangeBus>().notifyAll();
}

Status SceneDrivers::update(double time)
{
  CDCC_TRY(s_->rt->busyIfEvaluating("SceneDrivers::update"));
  Library *lib = s_->lib.get();
  if (!lib) {
    return failed("the Library is gone");
  }
  return s_->ensure(*lib, time);
}

Vector<DriverDiagnostic> SceneDrivers::diagnostics()
{
  Library *lib = s_->lib.get();
  if (lib && !s_->rt->evaluating()) {
    (void)s_->sync(*lib, false);
  }
  return s_->diagnostics;
}

const GraphDef *SceneDrivers::generated() const
{
  return s_->def.get();
}

const GraphInstance *SceneDrivers::instance() const
{
  return s_->inst.get();
}

NodeId SceneDrivers::transformNode(const BlockId &object) const
{
  const NodeId *n = s_->nodeOf.lookup_ptr(object);
  return n ? *n : NodeId();
}

const TransformState *SceneDrivers::generatedState(const BlockId &object) const
{
  const SocketHandle *h = s_->stateOf.lookup_ptr(object);
  const GraphValue *v = h && s_->inst ? s_->inst->value(*h) : nullptr;
  return v ? v->payload<TransformState>() : nullptr;
}

int SceneDrivers::transformCount() const
{
  return int(s_->nodeOf.size());
}

int SceneDrivers::rebuilds() const
{
  return s_->rebuilds;
}

Status SceneDrivers::prepare(const Library &lib, double time, Driven &driven)
{
  Library *own = s_->lib.get();
  driven.ids.clear();
  if (own != &lib) {
    driven.all = true;
    return failed("SceneDrivers: another Library");
  }
  const Status s = s_->ensure(*own, time);
  driven.all = s_->inner != nullptr;
  driven.version = s_->drivenVersion;
  for (auto &pair : s_->drivesOf) {
    driven.ids.append(pair.key);
  }
  return s;
}

Status SceneDrivers::overridesOf(const Library &lib,
                                 BlockHandle object,
                                 const TransformType &type,
                                 double time,
                                 ChannelOverrides &out)
{
  if (s_->inner) {
    CDCC_TRY(s_->inner->overridesOf(lib, object, type, time, out));
  }
  Library *own = s_->lib.get();
  const Datablock *b = lib.get(object);
  if (own != &lib || !b) {
    return Status();
  }
  const Status current = s_->ensure(*own, time);
  const Vector<int> *entries = s_->drivesOf.lookup_ptr(b->id());
  if (!entries) {
    return Status();
  }
  if (!current.ok()) {
    return failed(std::string("its drivers: ") + current.message());
  }
  if (const std::string *why = s_->problems.lookup_ptr(b->id())) {
    return failed(*why);
  }
  for (int i : *entries) {
    const DriveEntry &e = s_->drives[i];
    DriveKind kind = DriveKind::None;
    const int32_t at = offsetOf(type, e.channel.c_str(), kind);
    if (at < 0) {
      return failed("its transform type has no channel '" +
                    std::string(e.channel.c_str()) + "', which a graph drives");
    }
    if (kind != e.kind) {
      return failed("its channel '" + std::string(e.channel.c_str()) + "' is a " +
                    kindName(kind) + ", and a graph drives it with a " +
                    kindName(e.kind));
    }
    if (!e.ok) {
      return failed("the driver of '" + std::string(e.channel.c_str()) +
                    "' failed: " + e.error);
    }
    for (int c = 0; c < kindChannels(kind); c++) {
      out.set(at + c, e.values[c]);
    }
  }
  return Status();
}

} // namespace coredcc
