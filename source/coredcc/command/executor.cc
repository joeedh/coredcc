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

/* Marks a slot call other than exec, so the slot can't call back into the executor. */
class SlotScope {
 public:
  explicit SlotScope(int &depth) : depth_(depth)
  {
    depth_++;
  }
  ~SlotScope()
  {
    depth_--;
  }

 private:
  int &depth_;
};

/* Binds a Context with no executor to `ex` for one call. */
class Binding {
 public:
  Binding(Context &ctx, CommandExecutor *ex) : ctx_(ctx)
  {
    if (!ctx.executor()) {
      ctx.setExecutor(ex);
      bound_ = true;
    }
    ok_ = ctx.executor() == ex;
  }
  ~Binding()
  {
    if (bound_) {
      ctx_.setExecutor(nullptr);
    }
  }
  Status status() const
  {
    return ok_ ? Status()
               : Status::error(StatusCode::FailedPrecondition,
                               "the Context belongs to another executor");
  }

 private:
  Context &ctx_;
  bool bound_ = false;
  bool ok_ = false;
};

Status combine(const Status &first, const char *joint, const Status &second)
{
  return Status::error(first.code(), (std::string(first.message()) + joint +
                                      second.message())
                                         .c_str());
}

/* def.newInputs() with the caller's set values copied in. */
Result<PropertyBag> conformInputs(const CommandDef &def, const PropertyBag &given)
{
  PropertyBag bag = def.newInputs();
  for (int i = 0; i < given.size(); i++) {
    const char *name = given.def(i).apiName().c_str();
    const int at = bag.indexOf(name);
    if (at < 0) {
      return Status::error(StatusCode::InvalidArgument,
                           ("'" + std::string(def.name().c_str()) + "' has no input '" +
                            name + "'")
                               .c_str());
    }
    if (!given.wasSet(i)) {
      continue;
    }
    if (Status s = bag.setAt(at, given.value(i)); !s.ok()) {
      return Status::error(StatusCode::InvalidArgument,
                           ("'" + std::string(def.name().c_str()) + "' input '" + name +
                            "': " + s.message())
                               .c_str());
    }
  }
  return bag;
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
    Status s = ex ? ex->execFrame(ctx, *this) : exec(ctx);
    if (!s.ok()) {
      rollBackSubs(ctx);
    }
    return s;
  }
  int done = 0;
  Status s;
  for (; done < int(subs_.size()); done++) {
    s = subs_[done]->redo(ctx);
    if (!s.ok()) {
      break;
    }
  }
  if (s.ok()) {
    BagScope scope(ctx, *this);
    s = cmd_.get().redo(ctx);
  }
  if (!s.ok()) {
    for (int i = done - 1; i >= 0; i--) {
      subs_[i]->undo(ctx);
    }
  }
  return s;
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

void CommandRecord::adoptBags(CommandRecord &from)
{
  std::swap(inputs_, from.inputs_);
  std::swap(outputs_, from.outputs_);
}

/* ---- LinearHistory ---- */

namespace {

/* Marks the history as inside a record's slot; nested calls are refused. */
class HistoryCall {
 public:
  explicit HistoryCall(bool &flag) : flag_(flag)
  {
    CDCC_CHECK(!flag_, "LinearHistory called from inside one of its records' slots");
    flag_ = true;
  }
  ~HistoryCall()
  {
    flag_ = false;
  }

 private:
  bool &flag_;
};

} // namespace

LinearHistory::~LinearHistory()
{
  clear();
}

void LinearHistory::drop(CommandRecord *r)
{
  used_ -= r->cachedSize_;
  r->discard();
  litestl::alloc::Delete(r);
}

void LinearHistory::truncateRedo()
{
  while (int(entries_.size()) > cursor_) {
    CommandRecord *r = entries_.last();
    entries_.pop_back();
    drop(r);
  }
}

void LinearHistory::push(RecordPtr rec)
{
  HistoryCall call(inCall_);
  truncateRedo();
  if (headFoldable_ && cursor_ > floor_ && rec->command().hasFold() && rec->subCount() == 0) {
    CommandRecord *head = entries_[cursor_ - 1];
    const uint64_t key = rec->command().foldKey();
    if (key != 0 && head->subCount() == 0 && head->command().hasFold() &&
        head->command().foldKey() == key && head->command().foldFrom(rec->command()))
    {
      head->adoptBags(*rec);
      rec->discard();
      used_ -= head->cachedSize_;
      head->cachedSize_ = head->memSize();
      used_ += head->cachedSize_;
      enforceBudget();
      return;
    }
  }
  rec->cachedSize_ = rec->memSize();
  used_ += rec->cachedSize_;
  entries_.append(rec.release());
  cursor_ = int(entries_.size());
  headFoldable_ = true;
  enforceBudget();
}

void LinearHistory::pushBarrier()
{
  HistoryCall call(inCall_);
  truncateRedo();
  floor_ = cursor_;
  headFoldable_ = false;
}

void LinearHistory::enforceBudget()
{
  /* Drop the oldest, never the newest. */
  while (used_ > budget_ && entries_.size() > 1 && cursor_ > 1) {
    CommandRecord *oldest = entries_[0];
    entries_.remove_at(0);
    drop(oldest);
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
  if (inCall_) {
    return Status::error(StatusCode::Busy, "LinearHistory::undo from inside a slot");
  }
  if (!canUndo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to undo");
  }
  HistoryCall call(inCall_);
  headFoldable_ = false;
  entries_[--cursor_]->undo(ctx);
  return Status();
}

Status LinearHistory::redo(Context &ctx)
{
  if (inCall_) {
    return Status::error(StatusCode::Busy, "LinearHistory::redo from inside a slot");
  }
  if (!canRedo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to redo");
  }
  HistoryCall call(inCall_);
  headFoldable_ = false;
  Status s = entries_[cursor_]->redo(ctx);
  if (!s.ok()) {
    truncateRedo(); /* the failed command (already rolled back) and what depends on it */
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
  used_ -= h->cachedSize_;
  h->cachedSize_ = 0;
  cursor_--;
  headFoldable_ = false;
  return RecordPtr(h);
}

void LinearHistory::clear()
{
  HistoryCall call(inCall_);
  cursor_ = 0;
  truncateRedo();
  floor_ = 0;
  headFoldable_ = false;
}

void LinearHistory::setMemoryBudget(uint64_t bytes)
{
  HistoryCall call(inCall_);
  budget_ = bytes;
  enforceBudget();
}

uint64_t LinearHistory::memoryUsed() const
{
  return used_;
}

Status LinearHistory::validate() const
{
  if (cursor_ < 0 || cursor_ > int(entries_.size()) || floor_ < 0 || floor_ > cursor_) {
    return Status::error(StatusCode::Internal, "LinearHistory: cursor or floor out of range");
  }
  uint64_t total = 0;
  for (const CommandRecord *r : entries_) {
    if (!r) {
      return Status::error(StatusCode::Internal, "LinearHistory: a null entry");
    }
    total += r->cachedSize_;
  }
  if (total != used_) {
    return Status::error(StatusCode::Internal, "LinearHistory: the memory total drifted");
  }
  return Status();
}

/* ---- CommandExecutor ---- */

CommandExecutor::CommandExecutor(Runtime *rt, UndoHistory *history) : rt_(*rt), history_(history)
{
}

CommandExecutor::~CommandExecutor()
{
  CDCC_CHECK(stack_.size() == 0 && slotDepth_ == 0, "CommandExecutor destroyed while a command runs");
  if (open_) {
    /* Cancel: leave neither the live edit nor the collection block behind. A sticky
     * error from the undo stays pending for the next caller. */
    SlotScope slot(slotDepth_);
    if (openCtx_) {
      open_->undo(*openCtx_);
    }
    open_->discard();
    open_.reset();
    if (openLib_) {
      openLib_->endNoCollect();
    }
  }
}

Status CommandExecutor::busy(const char *what) const
{
  return Status::error(StatusCode::Busy,
                       (std::string(what) + ": an interactive command (" +
                        open_->def().name().c_str() + ") is open")
                           .c_str());
}

Status CommandExecutor::refuseNested(const char *what)
{
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(what) + " inside a command: only exec may call runSub")
                             .c_str());
  }
  if (rt_.hasStickyError()) {
    Status stray = rt_.takeStickyError();
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(what) + ": an earlier error was never collected: " +
                          stray.message())
                             .c_str());
  }
  return Status();
}

Status CommandExecutor::collect(Status s)
{
  if (!rt_.hasStickyError()) {
    return s;
  }
  Status sticky = rt_.takeStickyError();
  return s.ok() ? sticky : combine(s, "; also: ", sticky);
}

void CommandExecutor::discardRecord(CommandRecord &rec)
{
  SlotScope slot(slotDepth_);
  rec.discard();
}

Result<RecordPtr> CommandExecutor::start(Context &ctx,
                                         const CommandDef &def,
                                         Owned<Command> cmd,
                                         PropertyBag inputs)
{
  if (ctx.executor() != this) {
    return Status::error(StatusCode::FailedPrecondition,
                         "the Context belongs to another executor");
  }
  Result<PropertyBag> bag = conformInputs(def, inputs);
  if (!bag.ok()) {
    if (cmd) {
      SlotScope slot(slotDepth_);
      cmd.get().discard();
    }
    return bag.status();
  }
  const bool fromFactory = !cmd;
  if (!cmd) {
    SlotScope slot(slotDepth_);
    Result<Owned<Command>> made = def.instantiate(rt_);
    if (!made.ok()) {
      return collect(made.status());
    }
    cmd = std::move(made).value();
  }
  RecordPtr rec(litestl::alloc::New<CommandRecord>("coredcc::CommandRecord", &def,
                                                   std::move(cmd), std::move(bag).value()));
  rec->fromFactory_ = fromFactory;
  Status ok;
  {
    SlotScope slot(slotDepth_);
    BagScope scope(ctx, *rec);
    ok = rec->command().canRun(ctx);
  }
  ok = collect(ok);
  if (!ok.ok()) {
    discardRecord(*rec);
    return ok;
  }
  return rec;
}

Status CommandExecutor::execFrame(Context &ctx, CommandRecord &rec)
{
  const int savedDepth = slotDepth_;
  slotDepth_ = 0;
  stack_.append(&rec);
  Status s = rec.exec(ctx);
  stack_.pop_back();
  slotDepth_ = savedDepth;
  return s;
}

Status CommandExecutor::execRecord(Context &ctx, CommandRecord &rec)
{
  Status s = collect(execFrame(ctx, rec));
  if (!s.ok()) {
    {
      SlotScope slot(slotDepth_);
      rec.rollBackSubs(ctx);
      rec.discard();
    }
    s = collect(s);
  }
  return s;
}

Result<PropertyBag> CommandExecutor::finish(Context &, RecordPtr rec)
{
  PropertyBag outputs = rec->outputs();
  const CommandDef &def = rec->def();
  rt_.ext<LastUsedStore>().remember(def, rec->inputs());
  {
    SlotScope slot(slotDepth_);
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
  }
  if (rt_.hasStickyError()) {
    /* From the history's upkeep (memSize, folding, discarding old entries). */
    Status s = rt_.takeStickyError();
    return Status::error(s.code(), ("'" + std::string(def.name().c_str()) +
                                    "' ran and was recorded, but the history's upkeep "
                                    "failed: " + s.message())
                                       .c_str());
  }
  return outputs;
}

Result<PropertyBag> CommandExecutor::runInstance(Context &ctx,
                                                 const CommandDef &def,
                                                 Owned<Command> cmd,
                                                 PropertyBag inputs)
{
  if (Status s = refuseNested(("run(" + std::string(def.name().c_str()) + ")").c_str());
      !s.ok()) {
    return s;
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
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
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
  if (!running() || slotDepth_ > 0) {
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
  if (open_ && !running() && slotDepth_ == 0) {
    return cancel(ctx); /* before the stray-error check: cancel collects the live phase's */
  }
  CDCC_TRY(refuseNested("undo"));
  if (!history_) {
    return Status::error(StatusCode::FailedPrecondition, "no undo history");
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  Status s;
  {
    SlotScope slot(slotDepth_);
    s = history_->undo(ctx);
  }
  return collect(s);
}

Status CommandExecutor::redo(Context &ctx)
{
  if (open_ && !running() && slotDepth_ == 0) {
    return cancel(ctx); /* before the stray-error check: cancel collects the live phase's */
  }
  CDCC_TRY(refuseNested("redo"));
  if (!history_) {
    return Status::error(StatusCode::FailedPrecondition, "no undo history");
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  Status s;
  {
    SlotScope slot(slotDepth_);
    s = history_->redo(ctx);
  }
  return collect(s);
}

Status CommandExecutor::rerunHead(Context &ctx, PropertyBag newInputs)
{
  CDCC_TRY(refuseNested("rerunHead"));
  if (open_) {
    return busy("rerunHead");
  }
  CommandRecord *head = history_ ? history_->head() : nullptr;
  if (!head) {
    return Status::error(StatusCode::FailedPrecondition,
                         "rerunHead: no command to re-run (or one was undone since)");
  }
  const CommandDef &def = head->def();
  const std::string id = def.name().c_str();
  if (def.has(CommandFlags::Interactive)) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("rerunHead: '" + id + "' is interactive; its values came from the drag")
                             .c_str());
  }
  if (!head->fromFactory()) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("rerunHead: '" + id +
                          "' was run from an instance, so its inputs can't re-run it")
                             .c_str());
  }
  Result<PropertyBag> fresh = conformInputs(def, newInputs);
  if (!fresh.ok()) {
    return fresh.status();
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());

  RecordPtr old = history_->popHead();
  {
    SlotScope slot(slotDepth_);
    old->undo(ctx);
  }
  if (Status u = collect(Status()); !u.ok()) {
    discardRecord(*old);
    return Status::error(u.code(), ("rerunHead: undoing the previous run failed, and it was "
                                    "dropped: " + std::string(u.message()))
                                       .c_str());
  }

  Result<PropertyBag> out = run(ctx, def, std::move(fresh).value());
  if (out.ok()) {
    discardRecord(*old);
    return Status();
  }
  Status r;
  {
    SlotScope slot(slotDepth_);
    r = old->redo(ctx);
  }
  r = collect(r);
  if (!r.ok()) {
    discardRecord(*old);
    return combine(out.status(), "; restoring the previous run also failed: ", r);
  }
  {
    SlotScope slot(slotDepth_);
    history_->push(std::move(old));
  }
  return collect(out.status());
}

Status CommandExecutor::beginInteractive(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  CDCC_TRY(refuseNested("beginInteractive"));
  if (open_) {
    return busy(def.name().c_str());
  }
  if (!def.has(CommandFlags::Interactive)) {
    return Status::error(StatusCode::FailedPrecondition,
                         ("'" + std::string(def.name().c_str()) + "' isn't interactive").c_str());
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  Result<RecordPtr> rec = start(ctx, def, Owned<Command>(), std::move(inputs));
  if (!rec.ok()) {
    return rec.status();
  }
  CDCC_TRY(execRecord(ctx, **rec));
  open_ = std::move(rec).value();
  openCtx_ = &ctx;
  openLib_ = ctx.library();
  if (openLib_) {
    openLib_->beginNoCollect();
  }
  return Status();
}

Command *CommandExecutor::interactiveCommand()
{
  return open_ ? &open_->command() : nullptr;
}

Status CommandExecutor::commit(Context &ctx)
{
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition, "commit inside a command");
  }
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition, "commit: no interactive command");
  }
  if (rt_.hasStickyError()) {
    /* The live phase failed: what it left applied is unknown, so undo it all. */
    Status live = rt_.takeStickyError();
    Status c = cancel(ctx);
    Status s = Status::error(live.code(), ("commit: the live edit failed, so the command "
                                           "was cancelled: " + std::string(live.message()))
                                              .c_str());
    return c.ok() ? s : combine(s, "; also: ", c);
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  if (openLib_) {
    openLib_->endNoCollect();
  }
  openLib_ = nullptr;
  openCtx_ = nullptr;
  RecordPtr rec = std::move(open_);
  Result<PropertyBag> out = finish(ctx, std::move(rec));
  return out.ok() ? Status() : out.status();
}

Status CommandExecutor::cancel(Context &ctx)
{
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition, "cancel inside a command");
  }
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition, "cancel: no interactive command");
  }
  Binding bind(ctx, this);
  RecordPtr rec = std::move(open_);
  {
    SlotScope slot(slotDepth_);
    rec->undo(ctx);
    rec->discard();
  }
  if (openLib_) {
    openLib_->endNoCollect();
  }
  openLib_ = nullptr;
  openCtx_ = nullptr;
  return collect(Status());
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
