#include "coredcc/command/executor.h"

#include "coredcc/command/last_used.h"
#include "coredcc/base/text.h"
#include "coredcc/command/literal.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/invocation.h"

#include <string>

namespace coredcc {

using litestl::util::Vector;

namespace {

/* Points ctx's command bags (and executor) at one record for the length of a slot
 * call, restoring the outer ones after: sub-commands nest. */
class BagScope {
 public:
  BagScope(Context &ctx, CommandRecord &rec)
      : ctx_(ctx), inputs_(ctx.inputs()), outputs_(ctx.outputs())
  {
    ctx.setCommandBags(&rec.inputs(), &rec.outputs());
  }
  ~BagScope()
  {
    ctx_.setCommandBags(inputs_, outputs_);
  }

 private:
  Context &ctx_;
  const PropertyBag *inputs_;
  PropertyBag *outputs_;
};

Status combine(const Status &first, const Status &second)
{
  return Status::error(first.code(), (std::string(first.message()) +
                                      "; restoring the previous inputs also failed: " +
                                      second.message())
                                         .c_str());
}

} // namespace

/* ---- CommandRecord ---- */

CommandRecord::CommandRecord(const CommandDef *def, Owned<Command> cmd, PropertyBag inputs)
    : def_(def), cmd_(std::move(cmd)), inputs_(std::move(inputs)), outputs_(def->newOutputs())
{
}

CommandRecord::~CommandRecord()
{
  for (CommandRecord *s : subs_) {
    litestl::alloc::Delete(s);
  }
}

void RecordDeleter::operator()(CommandRecord *r) const
{
  litestl::alloc::Delete(r);
}

uint64_t CommandRecord::memSize() const
{
  uint64_t n = const_cast<Owned<Command> &>(cmd_).get().memSize();
  for (const CommandRecord *s : subs_) {
    n += s->memSize();
  }
  return n;
}

Status CommandRecord::exec(Context &ctx)
{
  BagScope scope(ctx, *this);
  return cmd_.get().exec(ctx);
}

void CommandRecord::undo(Context &ctx)
{
  for (int i = int(subs_.size()) - 1; i >= 0; i--) {
    subs_[i]->undo(ctx);
  }
  BagScope scope(ctx, *this);
  cmd_.get().undo(ctx);
}

Status CommandRecord::redo(Context &ctx)
{
  if (!cmd_.get().hasRedo()) {
    for (CommandRecord *s : subs_) {
      s->discard();
      litestl::alloc::Delete(s);
    }
    subs_.clear();
    /* exec runs its sub-commands again; ctx's executor must collect them. */
    CommandExecutor *ex = ctx.executor();
    if (ex) {
      ex->stack_.append(this);
    }
    Status s = exec(ctx);
    if (ex) {
      ex->stack_.pop_back();
    }
    return s;
  }
  for (CommandRecord *s : subs_) {
    CDCC_TRY(s->redo(ctx));
  }
  BagScope scope(ctx, *this);
  return cmd_.get().redo(ctx);
}

void CommandRecord::rollBackSubs(Context &ctx)
{
  for (int i = int(subs_.size()) - 1; i >= 0; i--) {
    subs_[i]->undo(ctx);
    subs_[i]->discard();
    litestl::alloc::Delete(subs_[i]);
  }
  subs_.clear();
}

void CommandRecord::discard()
{
  for (CommandRecord *s : subs_) {
    s->discard();
  }
  cmd_.get().discard();
}

/* ---- LinearHistory ---- */

LinearHistory::~LinearHistory()
{
  clear();
}

void LinearHistory::truncateRedo()
{
  while (int(entries_.size()) > cursor_) {
    CommandRecord *r = entries_.last();
    entries_.pop_back();
    r->discard();
    litestl::alloc::Delete(r);
  }
}

void LinearHistory::push(RecordPtr rec)
{
  truncateRedo();
  if (headFoldable_ && cursor_ > floor_ && rec->command().hasFold()) {
    CommandRecord *head = entries_[cursor_ - 1];
    const uint64_t key = rec->command().foldKey();
    if (key != 0 && head->command().hasFold() && head->command().foldKey() == key &&
        head->command().foldFrom(rec->command()))
    {
      rec->discard();
      enforceBudget();
      return;
    }
  }
  entries_.append(rec.release());
  cursor_ = int(entries_.size());
  headFoldable_ = true;
  enforceBudget();
}

void LinearHistory::pushBarrier()
{
  truncateRedo();
  floor_ = cursor_;
  headFoldable_ = false;
}

void LinearHistory::enforceBudget()
{
  uint64_t used = memoryUsed();
  /* Drop the oldest, never the newest. */
  while (used > budget_ && entries_.size() > 1 && cursor_ > 1) {
    CommandRecord *oldest = entries_[0];
    used -= oldest->memSize();
    oldest->discard();
    litestl::alloc::Delete(oldest);
    entries_.remove_at(0);
    cursor_--;
    floor_ = floor_ > 0 ? floor_ - 1 : 0;
  }
}

bool LinearHistory::canUndo() const
{
  return cursor_ > floor_;
}

bool LinearHistory::canRedo() const
{
  return cursor_ < int(entries_.size());
}

Status LinearHistory::undo(Context &ctx)
{
  if (!canUndo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to undo");
  }
  headFoldable_ = false;
  entries_[--cursor_]->undo(ctx);
  return Status();
}

Status LinearHistory::redo(Context &ctx)
{
  if (!canRedo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to redo");
  }
  headFoldable_ = false;
  Status s = entries_[cursor_]->redo(ctx);
  if (!s.ok()) {
    truncateRedo(); /* the failed command and what depends on it */
    return s;
  }
  cursor_++;
  return Status();
}

CommandRecord *LinearHistory::head()
{
  return headFoldable_ && cursor_ > floor_ ? entries_[cursor_ - 1] : nullptr;
}

RecordPtr LinearHistory::popHead()
{
  CommandRecord *h = head();
  if (!h) {
    return RecordPtr();
  }
  entries_.pop_back();
  cursor_--;
  headFoldable_ = false;
  return RecordPtr(h);
}

void LinearHistory::clear()
{
  cursor_ = 0;
  truncateRedo();
  floor_ = 0;
  headFoldable_ = false;
}

void LinearHistory::setMemoryBudget(uint64_t bytes)
{
  budget_ = bytes;
  enforceBudget();
}

uint64_t LinearHistory::memoryUsed() const
{
  uint64_t n = 0;
  for (const CommandRecord *r : entries_) {
    n += r->memSize();
  }
  return n;
}

Status LinearHistory::validate() const
{
  if (cursor_ < 0 || cursor_ > int(entries_.size()) || floor_ < 0 || floor_ > cursor_) {
    return Status::error(StatusCode::Internal, "LinearHistory: cursor or floor out of range");
  }
  for (const CommandRecord *r : entries_) {
    if (!r) {
      return Status::error(StatusCode::Internal, "LinearHistory: a null entry");
    }
  }
  return Status();
}

/* ---- CommandExecutor ---- */

CommandExecutor::CommandExecutor(Runtime *rt, UndoHistory *history) : rt_(*rt), history_(history)
{
}

CommandExecutor::~CommandExecutor()
{
  CDCC_CHECK(stack_.size() == 0, "CommandExecutor destroyed while a command runs");
  if (open_) {
    open_->discard();
  }
}

Status CommandExecutor::busy(const char *what) const
{
  return Status::error(StatusCode::Busy,
                       (std::string(what) + ": an interactive command (" +
                        open_->def().name().c_str() + ") is open")
                           .c_str());
}

Result<RecordPtr> CommandExecutor::start(Context &ctx,
                                         const CommandDef &def,
                                         Owned<Command> cmd,
                                         PropertyBag inputs)
{
  if (!cmd) {
    Result<Owned<Command>> made = def.instantiate(rt_);
    if (!made.ok()) {
      return made.status();
    }
    cmd = std::move(made).value();
  }
  if (!ctx.executor()) {
    ctx.setExecutor(this);
  }
  CDCC_CHECK(ctx.executor() == this, "the Context belongs to another executor");
  RecordPtr rec(litestl::alloc::New<CommandRecord>("coredcc::CommandRecord", &def,
                                                   std::move(cmd), std::move(inputs)));
  {
    BagScope scope(ctx, *rec);
    CDCC_TRY(rec->command().canRun(ctx));
  }
  return rec;
}

Status CommandExecutor::execRecord(Context &ctx, CommandRecord &rec)
{
  stack_.append(&rec);
  Status s = rec.exec(ctx);
  stack_.pop_back();
  if (s.ok() && rt_.hasStickyError()) {
    s = rt_.takeStickyError();
  }
  if (!s.ok()) {
    rec.rollBackSubs(ctx);
    rec.discard();
  }
  return s;
}

Result<PropertyBag> CommandExecutor::finish(Context &, RecordPtr rec)
{
  PropertyBag outputs = rec->outputs();
  const CommandDef &def = rec->def();
  rt_.ext<LastUsedStore>().remember(def, rec->inputs());
  if (!history_ || def.has(CommandFlags::NoUndo)) {
    rec->discard();
  }
  else if (def.has(CommandFlags::UndoBarrier)) {
    rec->discard();
    history_->pushBarrier();
  }
  else {
    history_->push(std::move(rec));
  }
  return outputs;
}

Result<PropertyBag> CommandExecutor::runInstance(Context &ctx,
                                                 const CommandDef &def,
                                                 Owned<Command> cmd,
                                                 PropertyBag inputs)
{
  if (running()) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("run(" + std::string(def.name().c_str()) +
                          ") inside a command: use runSub")
                             .c_str());
  }
  if (open_) {
    return busy(def.name().c_str());
  }
  if (def.has(CommandFlags::Interactive)) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("'" + std::string(def.name().c_str()) +
                          "' is interactive: use beginInteractive")
                             .c_str());
  }
  Result<RecordPtr> rec = start(ctx, def, std::move(cmd), std::move(inputs));
  if (!rec.ok()) {
    return rec.status();
  }
  CDCC_TRY(execRecord(ctx, **rec));
  return finish(ctx, std::move(rec).value());
}

Result<PropertyBag> CommandExecutor::run(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  return runInstance(ctx, def, Owned<Command>(), std::move(inputs));
}

Result<PropertyBag> CommandExecutor::run(Context &ctx, std::string_view id, PropertyBag inputs)
{
  const CommandDef *def = rt_.ext<CommandRegistry>().find(id);
  if (!def) {
    return rt_.ext<CommandRegistry>().unknown(id);
  }
  return run(ctx, *def, std::move(inputs));
}

Result<PropertyBag> CommandExecutor::runSub(Context &ctx, Owned<Command> cmd, const CommandDef &def)
{
  return subRun(ctx, def, std::move(cmd), def.newInputs());
}

Result<PropertyBag> CommandExecutor::runSub(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  return subRun(ctx, def, Owned<Command>(), std::move(inputs));
}

Result<PropertyBag> CommandExecutor::subRun(Context &ctx,
                                            const CommandDef &def,
                                            Owned<Command> cmd,
                                            PropertyBag inputs)
{
  if (!running()) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("runSub(" + std::string(def.name().c_str()) +
                          ") outside a command's exec")
                             .c_str());
  }
  Result<RecordPtr> rec = start(ctx, def, std::move(cmd), std::move(inputs));
  if (!rec.ok()) {
    return rec.status();
  }
  CommandRecord *parent = stack_.last();
  CDCC_TRY(execRecord(ctx, **rec));
  PropertyBag outputs = (*rec)->outputs();
  parent->subs_.append(rec->release());
  return outputs;
}

Status CommandExecutor::undo(Context &ctx)
{
  if (running()) {
    return Status::error(StatusCode::Busy, "undo inside a command");
  }
  if (open_) {
    return cancel(ctx);
  }
  if (!history_) {
    return Status::error(StatusCode::FailedPrecondition, "no undo history");
  }
  if (!ctx.executor()) {
    ctx.setExecutor(this);
  }
  Status s = history_->undo(ctx);
  if (s.ok() && rt_.hasStickyError()) {
    s = rt_.takeStickyError();
  }
  return s;
}

Status CommandExecutor::redo(Context &ctx)
{
  if (running()) {
    return Status::error(StatusCode::Busy, "redo inside a command");
  }
  if (open_) {
    return cancel(ctx);
  }
  if (!history_) {
    return Status::error(StatusCode::FailedPrecondition, "no undo history");
  }
  if (!ctx.executor()) {
    ctx.setExecutor(this);
  }
  Status s = history_->redo(ctx);
  if (s.ok() && rt_.hasStickyError()) {
    s = rt_.takeStickyError();
  }
  return s;
}

Status CommandExecutor::rerunHead(Context &ctx, PropertyBag newInputs)
{
  if (running() || open_) {
    return Status::error(StatusCode::Busy, "rerunHead while a command runs");
  }
  CommandRecord *head = history_ ? history_->head() : nullptr;
  if (!head) {
    return Status::error(StatusCode::FailedPrecondition,
                         "rerunHead: no command to re-run (or one was undone since)");
  }
  RecordPtr old = history_->popHead();
  const CommandDef &def = old->def();
  PropertyBag oldInputs = old->inputs();
  old->undo(ctx);
  old->discard();
  old.reset();

  Result<PropertyBag> fresh = run(ctx, def, std::move(newInputs));
  if (fresh.ok()) {
    return Status();
  }
  Result<PropertyBag> restored = run(ctx, def, std::move(oldInputs));
  if (restored.ok()) {
    return fresh.status();
  }
  return combine(fresh.status(), restored.status());
}

Status CommandExecutor::beginInteractive(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  if (running()) {
    return Status::error(StatusCode::FailedPrecondition, "beginInteractive inside a command");
  }
  if (open_) {
    return busy(def.name().c_str());
  }
  if (!def.has(CommandFlags::Interactive)) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("'" + std::string(def.name().c_str()) + "' isn't interactive").c_str());
  }
  Result<RecordPtr> rec = start(ctx, def, Owned<Command>(), std::move(inputs));
  if (!rec.ok()) {
    return rec.status();
  }
  CDCC_TRY(execRecord(ctx, **rec));
  open_ = std::move(rec).value();
  if (ctx.library()) {
    ctx.library()->beginNoCollect();
  }
  return Status();
}

Command *CommandExecutor::interactiveCommand()
{
  return open_ ? &open_->command() : nullptr;
}

Status CommandExecutor::commit(Context &ctx)
{
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition, "commit: no interactive command");
  }
  if (ctx.library()) {
    ctx.library()->endNoCollect();
  }
  RecordPtr rec = std::move(open_);
  Result<PropertyBag> out = finish(ctx, std::move(rec));
  return out.ok() ? Status() : out.status();
}

Status CommandExecutor::cancel(Context &ctx)
{
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition, "cancel: no interactive command");
  }
  RecordPtr rec = std::move(open_);
  rec->undo(ctx);
  rec->discard();
  if (ctx.library()) {
    ctx.library()->endNoCollect();
  }
  return Status();
}

/* ---- invocation ---- */

Result<PropertyBag> inputsFromInvocation(Runtime &rt, const CommandDef &def, const Invocation &inv)
{
  PropertyBag bag = def.newInputs();
  rt.ext<LastUsedStore>().apply(def, bag);
  for (const InvocationArg &arg : inv.args) {
    const PropertyDef *in = def.findInput(arg.name.c_str());
    if (!in) {
      Status err = Status::error(StatusCode::NotFound,
                                 (std::string(def.name().c_str()) + " has no input '" +
                                  arg.name.c_str() + "' (at offset " +
                                  std::to_string(arg.offset) + ")")
                                     .c_str());
      Suggester sg(arg.name.c_str());
      for (int i = 0; i < def.inputCount(); i++) {
        sg.consider(def.inputDef(i).apiName().c_str());
      }
      sg.addTo(err);
      return err;
    }
    Result<PropValue> v = parseLiteral(*in, arg.value);
    if (!v.ok()) {
      return v.status();
    }
    CDCC_TRY(bag.set(arg.name.c_str(), *v));
  }
  return bag;
}

Result<PropertyBag> invoke(Context &ctx, std::string_view invocation)
{
  CommandExecutor *ex = ctx.executor();
  if (!ex) {
    return Status::error(StatusCode::FailedPrecondition, "invoke: the Context has no executor");
  }
  Result<Invocation> inv = parseInvocation(invocation);
  if (!inv.ok()) {
    return inv.status();
  }
  Runtime &rt = ctx.runtime();
  const CommandDef *def = rt.ext<CommandRegistry>().find(inv->command.c_str());
  if (!def) {
    return rt.ext<CommandRegistry>().unknown(inv->command.c_str());
  }
  Result<PropertyBag> inputs = inputsFromInvocation(rt, *def, *inv);
  if (!inputs.ok()) {
    return inputs.status();
  }
  return ex->run(ctx, *def, std::move(inputs).value());
}

} // namespace coredcc
