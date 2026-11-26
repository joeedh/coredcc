#include "coredcc/bind/run_facades.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/bind/detail.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/last_used.h"
#include "coredcc/parse/invocation.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::slotGate;
using detail::view;

struct RunInputs::Core {
  std::vector<std::pair<std::string, PropValue>> values;
};

struct InvocationChecks::Core {
  struct Entry {
    Path prefix;
    Invocation invocation;
    /** Nothing before the first call; "" if it can run. */
    std::optional<std::string> answer;
  };
  std::vector<Entry> entries;
};

RunInputs::RunInputs() : core_(std::make_shared<Core>())
{
}
InvocationChecks::InvocationChecks() : core_(std::make_shared<Core>())
{
}

template <class T> static litestl::binding::types::Struct<T> *shared(const char *name)
{
  static litestl::binding::types::Struct<T> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new litestl::binding::types::Struct<T>(name, sizeof(T));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

const litestl::binding::types::Struct<RunInputs> *RunInputs::defineBindings()
{
  return shared<RunInputs>("coredcc::RunInputs");
}

const litestl::binding::types::Struct<InvocationChecks> *
InvocationChecks::defineBindings()
{
  return shared<InvocationChecks>("coredcc::InvocationChecks");
}

RunInputs newRunInputs()
{
  return RunInputs();
}

Status runInputsSet(const RunInputs &inputs, const string &name, const PropValue &value)
{
  inputs.core().values.emplace_back(view(name), value);
  return Status();
}

/* ---- preparing ---- */

/* `values` into the bag: those of dynamic inputs last, as the others may decide about
 * them. */
static Status setInputs(const CommandDef &def, const RunInputs &inputs, PropertyBag &bag)
{
  for (bool dynamic : {false, true}) {
    for (const auto &[name, value] : inputs.core().values) {
      const int i = bag.indexOf(name);
      if (i < 0) {
        InvocationArg arg;
        arg.name = string(name.c_str());
        return coredcc::detail::unknownInput(def, arg);
      }
      if (bag.def(i).isDynamic() == dynamic) {
        CDCC_TRY(bag.setAt(i, value));
      }
    }
  }
  return Status();
}

/* A run: what it needs of the Context, and its bag. */
struct Prepared {
  const CommandDef *def = nullptr;
  PropertyBag inputs;
};

static Result<Prepared>
prepare(Context &ctx, const string &id, const RunInputs &inputs, bool lastUsed)
{
  const CommandRegistry &commands = ctx.runtime().ext<CommandRegistry>();
  Prepared out;
  out.def = commands.find(view(id));
  if (!out.def) {
    return commands.unknown(view(id));
  }
  out.inputs = out.def->newInputs();
  if (lastUsed) {
    ctx.runtime().ext<LastUsedStore>().apply(*out.def, out.inputs);
  }
  CDCC_TRY(setInputs(*out.def, inputs, out.inputs));
  return out;
}

static Result<CommandExecutor *> executorOf(Context &ctx, const char *what)
{
  if (!ctx.executor()) {
    return error(StatusCode::FailedPrecondition,
                 std::string(what) + ": the context has no executor");
  }
  return ctx.executor();
}

/* `fn()` under `prefix`. */
template <class Fn>
static auto under(Context &ctx, const string &prefix, Fn fn) -> decltype(fn())
{
  if (prefix.size() == 0) {
    PrefixScope scope(ctx, nullptr);
    return fn();
  }
  Result<Path> path = parsePath(view(prefix));
  if (!path.ok()) {
    return path.status();
  }
  PrefixScope scope(ctx, path->size() > 0 ? &*path : nullptr);
  return fn();
}

/* ---- running ---- */

BagResult runAt(Context &ctx,
                const string &prefix,
                const string &id,
                const RunInputs &inputs,
                bool lastUsed)
{
  if (Status s = slotGate(ctx, "runAt", {"exec"}); !s.ok()) {
    return failed<BagResult>(std::move(s));
  }
  if (Status s = executorOf(ctx, "runAt").status(); !s.ok()) {
    return failed<BagResult>(std::move(s));
  }
  Result<PropertyBag> r = under(ctx, prefix, [&]() -> Result<PropertyBag> {
    Result<Prepared> p = prepare(ctx, id, inputs, lastUsed);
    return p.ok() ? detail::runCommand(ctx, *p->def, std::move(p->inputs)) : p.status();
  });
  return r.ok() ? ok<BagResult>(std::move(r).value()) : failed<BagResult>(r.status());
}

Status canRunAt(Context &ctx,
                const string &prefix,
                const string &id,
                const RunInputs &inputs,
                bool lastUsed)
{
  CDCC_TRY(slotGate(ctx, "canRunAt", {}));
  CDCC_TRY(executorOf(ctx, "canRunAt").status());
  return under(ctx, prefix, [&]() -> Status {
    Result<Prepared> p = prepare(ctx, id, inputs, lastUsed);
    return p.ok() ? ctx.executor()->canRun(ctx, *p->def, std::move(p->inputs))
                  : p.status();
  });
}

Status canInvokeAt(Context &ctx, const string &prefix, const string &invocation)
{
  CDCC_TRY(slotGate(ctx, "canInvokeAt", {}));
  return under(ctx, prefix, [&]() -> Status { return canInvoke(ctx, view(invocation)); });
}

InvocationChecks newInvocationChecks()
{
  return InvocationChecks();
}

IntResult invocationChecksAdd(const InvocationChecks &checks,
                              const string &prefix,
                              const string &invocation)
{
  InvocationChecks::Core::Entry entry;
  if (prefix.size() > 0) {
    Result<Path> path = parsePath(view(prefix));
    if (!path.ok()) {
      return failed<IntResult>(path.status());
    }
    entry.prefix = std::move(path).value();
  }
  Result<Invocation> inv = parseInvocation(view(invocation));
  if (!inv.ok()) {
    return failed<IntResult>(inv.status());
  }
  entry.invocation = std::move(inv).value();
  checks.core().entries.push_back(std::move(entry));
  return ok<IntResult>(int64_t(checks.core().entries.size()) - 1);
}

StringResult invocationChecksRun(Context &ctx, const InvocationChecks &checks)
{
  if (Status s = slotGate(ctx, "invocationChecksRun", {}); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  JsonWriter out;
  out.beginArray();
  /* By index: a script's canRun may add more. */
  for (size_t i = 0; i < checks.core().entries.size(); i++) {
    const Path prefix = checks.core().entries[i].prefix;
    const Invocation invocation = checks.core().entries[i].invocation;
    PrefixScope scope(ctx, prefix.size() > 0 ? &prefix : nullptr);
    const Status s = canInvoke(ctx, invocation);
    std::string answer = s.message(); /* an error has one */
    if (checks.core().entries[i].answer == answer) {
      continue;
    }
    out.beginArray().integer(int64_t(i));
    if (s.ok()) {
      out.null();
    } else {
      out.string(answer);
    }
    out.endArray();
    checks.core().entries[i].answer = std::move(answer);
  }
  out.endArray();
  return ok<StringResult>(string(out.take().c_str()));
}

/* ---- interactive commands ---- */

Status beginInteractiveAt(Context &ctx,
                          const string &prefix,
                          const string &id,
                          const RunInputs &inputs,
                          bool lastUsed)
{
  CDCC_TRY(slotGate(ctx, "beginInteractiveAt", {}));
  CDCC_TRY(executorOf(ctx, "beginInteractiveAt").status());
  return under(ctx, prefix, [&]() -> Status {
    Result<Prepared> p = prepare(ctx, id, inputs, lastUsed);
    return p.ok() ? ctx.executor()->beginInteractive(ctx, *p->def, std::move(p->inputs))
                  : p.status();
  });
}

StringResult interactiveId(Context &ctx)
{
  if (Status s = detail::kernelGate(ctx, "interactiveId"); !s.ok()) {
    return failed<StringResult>(std::move(s));
  }
  Result<CommandExecutor *> ex = executorOf(ctx, "interactiveId");
  if (!ex.ok()) {
    return failed<StringResult>(ex.status());
  }
  const CommandDef *def = (*ex)->interactiveDef();
  return ok<StringResult>(def ? def->name() : string(""));
}

Status writeInteractive(Context &ctx, const string &path, const PropValue &value)
{
  CDCC_TRY(slotGate(ctx, "writeInteractive", {}));
  CDCC_TRY(executorOf(ctx, "writeInteractive").status());
  auto write = [&](Context &c, Command &) {
    return coredcc::setValue(c, view(path), value);
  };
  return ctx.executor()->updateInteractive(ctx, write);
}

Status commitInteractive(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "commitInteractive", {}));
  CDCC_TRY(executorOf(ctx, "commitInteractive").status());
  return ctx.executor()->commit(ctx);
}

Status cancelInteractive(Context &ctx)
{
  CDCC_TRY(slotGate(ctx, "cancelInteractive", {}));
  CDCC_TRY(executorOf(ctx, "cancelInteractive").status());
  return ctx.executor()->cancel(ctx);
}

/* ---- the last command ---- */

/* Null without one that can be run again. */
static Result<CommandRecord *> last(Context &ctx, const char *what)
{
  CDCC_TRY(slotGate(ctx, what, {}));
  Result<CommandExecutor *> ex = executorOf(ctx, what);
  if (!ex.ok()) {
    return ex.status();
  }
  CommandRecord *head = (*ex)->history() ? (*ex)->history()->head() : nullptr;
  const bool can =
      head && head->fromFactory() && !head->def().has(CommandFlags::Interactive);
  return can ? head : nullptr;
}

StringResult lastCommandId(Context &ctx)
{
  Result<CommandRecord *> head = last(ctx, "lastCommand");
  if (!head.ok()) {
    return failed<StringResult>(head.status());
  }
  return ok<StringResult>(*head ? (*head)->def().name() : string(""));
}

static Status noLast()
{
  return error(StatusCode::FailedPrecondition,
               "there is no command that can be run again");
}

BagResult lastCommandInputs(Context &ctx)
{
  Result<CommandRecord *> head = last(ctx, "lastCommand");
  if (!head.ok() || !*head) {
    return failed<BagResult>(head.ok() ? noLast() : head.status());
  }
  return ok<BagResult>(PropertyBag((*head)->inputs()));
}

Status rerunLast(Context &ctx, const RunInputs &inputs)
{
  Result<CommandRecord *> head = last(ctx, "rerunLast");
  if (!head.ok() || !*head) {
    return head.ok() ? noLast() : head.status();
  }
  PropertyBag bag = (*head)->inputs();
  CDCC_TRY(setInputs((*head)->def(), inputs, bag));
  return ctx.executor()->rerunHead(ctx, std::move(bag));
}

} // namespace coredcc::script
