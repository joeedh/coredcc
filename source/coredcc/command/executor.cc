#include "coredcc/command/executor.h"

#include "coredcc/api/resolve.h"
#include "coredcc/base/text.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/last_used.h"
#include "coredcc/command/literal.h"
#include "coredcc/lib/library.h"
#include "coredcc/parse/invocation.h"

#include <string>

namespace coredcc {

using litestl::util::Vector;

namespace {

/* Points ctx's command bags and path prefix at one record for the length of a slot
 * call, restoring the outer ones after: sub-commands nest. */
class BagScope {
public:
  BagScope(Context &ctx, CommandRecord &rec)
      : ctx_(ctx), inputs_(ctx.inputs()), outputs_(ctx.outputs()),
        prefix_(ctx, rec.prefix())
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
  PrefixScope prefix_;
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
  return Status::error(first.code(),
                       (std::string(first.message()) + joint + second.message()).c_str());
}

/* def.newInputs() with the caller's set values copied in. */
Result<PropertyBag> conformInputs(const CommandDef &def, const PropertyBag &given)
{
  PropertyBag bag = def.newInputs();
  for (int i : given.setOrder()) {
    const char *name = given.def(i).apiName().c_str();
    const int at = bag.indexOf(name);
    if (at < 0) {
      return Status::error(
          StatusCode::InvalidArgument,
          ("'" + std::string(def.name().c_str()) + "' has no input '" + name + "'")
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

CommandRecord::CommandRecord(const CommandDef *def,
                             Owned<Command> cmd,
                             PropertyBag inputs)
    : def_(def), cmd_(std::move(cmd)), inputs_(std::move(inputs)),
      outputs_(def->newOutputs())
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

/* `s`, with a sticky error set by the slot call folded in: a failed foreign slot only
 * sets the sticky error, and returns an ok Status. */
static Status withSticky(Context &ctx, Status s)
{
  Runtime &rt = ctx.runtime();
  if (!rt.hasStickyError()) {
    return s;
  }
  Status sticky = rt.takeStickyError();
  return s.ok() ? sticky : combine(s, "; also: ", sticky);
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
    Status s = withSticky(ctx, ex ? ex->execFrame(ctx, *this) : exec(ctx));
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
    s = withSticky(ctx, cmd_.get().redo(ctx));
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
  if (int(entries_.size()) == cursor_) {
    return;
  }
  const detail::HistoryDestroysScope together(*entries_.last()->rt_);
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
  if (headFoldable_ && cursor_ > floor_ && rec->command().hasFold() &&
      rec->subCount() == 0)
  {
    CommandRecord *head = entries_[cursor_ - 1];
    const uint64_t key = rec->command().foldKey();
    if (key != 0 && head->subCount() == 0 && head->command().hasFold() &&
        head->command().foldKey() == key && head->prefix_ == rec->prefix_ &&
        head->command().foldFrom(rec->command()))
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
  headValid_ = headFoldable_ = true;
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
  if (used_ <= budget_) {
    return;
  }
  const detail::HistoryDestroysScope together(*entries_.last()->rt_);
  /* Drop the oldest, never the newest. */
  while (used_ > budget_ && entries_.size() > 1 && cursor_ > 1) {
    CommandRecord *oldest = entries_[0];
    entries_.remove_at(0);
    drop(oldest);
    cursor_--;
    floor_ = floor_ > 0 ? floor_ - 1 : 0;
  }
}

void LinearHistory::measure(CommandRecord &rec)
{
  used_ -= rec.cachedSize_;
  rec.cachedSize_ = rec.memSize();
  used_ += rec.cachedSize_;
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
  CDCC_TRY(ctx.runtime().busyIfEvaluating("LinearHistory::undo"));
  if (inCall_) {
    return Status::error(StatusCode::Busy, "LinearHistory::undo from inside a slot");
  }
  if (!canUndo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to undo");
  }
  HistoryCall call(inCall_);
  headValid_ = headFoldable_ = false;
  Runtime &rt = ctx.runtime();
  const bool stray = rt.hasStickyError();
  CommandRecord *rec = entries_[--cursor_];
  rec->undo(ctx);
  if (!stray && rt.hasStickyError()) {
    /* Undo can't be rolled back, and what it left is unknown: redoing over it isn't safe.
     */
    const std::string note = "undo of '" + std::string(rec->def().name().c_str()) +
                             "' failed, so it and the commands after it were dropped";
    truncateRedo();
    Status s = rt.takeStickyError();
    return combine(s, "; ", Status::error(s.code(), note.c_str()));
  }
  measure(*rec);
  return Status();
}

Status LinearHistory::redo(Context &ctx)
{
  CDCC_TRY(ctx.runtime().busyIfEvaluating("LinearHistory::redo"));
  if (inCall_) {
    return Status::error(StatusCode::Busy, "LinearHistory::redo from inside a slot");
  }
  if (!canRedo()) {
    return Status::error(StatusCode::FailedPrecondition, "nothing to redo");
  }
  HistoryCall call(inCall_);
  headValid_ = headFoldable_ = false;
  Status s = entries_[cursor_]->redo(ctx);
  if (!s.ok()) {
    truncateRedo(); /* the failed command (already rolled back) and what depends on it */
    return s;
  }
  measure(*entries_[cursor_]);
  cursor_++;
  return Status();
}

CommandRecord *LinearHistory::head()
{
  return headValid_ && cursor_ > floor_ ? entries_[cursor_ - 1] : nullptr;
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
  headValid_ = headFoldable_ = false;
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
    return Status::error(StatusCode::Internal,
                         "LinearHistory: cursor or floor out of range");
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

CommandExecutor::CommandExecutor(Runtime *rt, UndoHistory *history)
    : rt_(*rt), history_(history)
{
}

CommandExecutor::~CommandExecutor()
{
  CDCC_CHECK(stack_.size() == 0 && slotDepth_ == 0,
             "CommandExecutor destroyed while a command runs");
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
    return Status::error(
        StatusCode::FailedPrecondition,
        (std::string(what) + " inside a command: only exec may call runSub").c_str());
  }
  if (rt_.hasStickyError()) {
    Status stray = rt_.takeStickyError();
    return Status::error(
        StatusCode::FailedPrecondition,
        (std::string(what) + ": an earlier error was never collected: " + stray.message())
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
  std::optional<StableRef> prefix;
  if (ctx.hasPrefix()) {
    Result<StableRef> address = canonicalize(ctx, Path());
    if (!address.ok()) {
      if (cmd) {
        SlotScope slot(slotDepth_);
        cmd.get().discard();
      }
      return Status::error(
          address.status().code(),
          (std::string("the path prefix: ") + address.status().message()).c_str());
    }
    prefix = std::move(address).value();
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
  RecordPtr rec(litestl::alloc::New<CommandRecord>(
      "coredcc::CommandRecord", &def, std::move(cmd), std::move(bag).value()));
  rec->rt_ = &rt_;
  rec->fromFactory_ = fromFactory;
  rec->prefix_ = std::move(prefix);
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
    } else if (def.has(CommandFlags::UndoBarrier)) {
      rec->discard();
      history_->pushBarrier();
    } else {
      history_->push(std::move(rec));
    }
  }
  if (rt_.hasStickyError()) {
    /* From the history's upkeep (memSize, folding, discarding old entries). */
    Status s = rt_.takeStickyError();
    return Status::error(s.code(),
                         ("'" + std::string(def.name().c_str()) +
                          "' ran and was recorded, but the history's upkeep "
                          "failed: " +
                          s.message())
                             .c_str());
  }
  return outputs;
}

Result<PropertyBag> CommandExecutor::runInstance(Context &ctx,
                                                 const CommandDef &def,
                                                 Owned<Command> cmd,
                                                 PropertyBag inputs)
{
  const std::string what = "run(" + std::string(def.name().c_str()) + ")";
  CDCC_TRY(rt_.busyIfEvaluating(what.c_str()));
  CDCC_TRY(refuseNested(what.c_str()));
  if (open_) {
    return busy(def.name().c_str());
  }
  if (def.has(CommandFlags::Interactive)) {
    return Status::error(
        StatusCode::FailedPrecondition,
        ("'" + std::string(def.name().c_str()) + "' is interactive: use beginInteractive")
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

Result<PropertyBag>
CommandExecutor::run(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  return runInstance(ctx, def, Owned<Command>(), std::move(inputs));
}

Result<PropertyBag>
CommandExecutor::run(Context &ctx, std::string_view id, PropertyBag inputs)
{
  const CommandDef *def = rt_.ext<CommandRegistry>().find(id);
  if (!def) {
    return rt_.ext<CommandRegistry>().unknown(id);
  }
  return run(ctx, *def, std::move(inputs));
}

Status CommandExecutor::canRun(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  CDCC_TRY(rt_.busyIfEvaluating("canRun"));
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition, "canRun inside a command");
  }
  if (rt_.hasStickyError()) {
    return Status::error(StatusCode::FailedPrecondition,
                         "canRun: an earlier error was never collected");
  }
  if (open_) {
    return busy(def.name().c_str());
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  Result<RecordPtr> rec = start(ctx, def, Owned<Command>(), std::move(inputs));
  if (!rec.ok()) {
    return rec.status();
  }
  discardRecord(**rec);
  return collect(Status());
}

Result<PropertyBag>
CommandExecutor::runSub(Context &ctx, Owned<Command> cmd, const CommandDef &def)
{
  return subRun(ctx, def, std::move(cmd), def.newInputs());
}

Result<PropertyBag>
CommandExecutor::runSub(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  return subRun(ctx, def, Owned<Command>(), std::move(inputs));
}

Result<PropertyBag> CommandExecutor::subRun(Context &ctx,
                                            const CommandDef &def,
                                            Owned<Command> cmd,
                                            PropertyBag inputs)
{
  CDCC_TRY(rt_.busyIfEvaluating("runSub"));
  if (!running() || slotDepth_ > 0) {
    return Status::error(
        StatusCode::FailedPrecondition,
        ("runSub(" + std::string(def.name().c_str()) + ") outside a command's exec")
            .c_str());
  }
  if (int(stack_.size()) >= kMaxNesting) {
    return Status::error(StatusCode::OutOfRange,
                         ("runSub(" + std::string(def.name().c_str()) +
                          "): commands nested deeper than " + std::to_string(kMaxNesting))
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
  CDCC_TRY(rt_.busyIfEvaluating("undo"));
  if (open_ && !running() && slotDepth_ == 0) {
    return cancel(
        ctx); /* before the stray-error check: cancel collects the live phase's */
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
  CDCC_TRY(rt_.busyIfEvaluating("redo"));
  if (open_ && !running() && slotDepth_ == 0) {
    return cancel(
        ctx); /* before the stray-error check: cancel collects the live phase's */
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
  CDCC_TRY(rt_.busyIfEvaluating("rerunHead"));
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
    return Status::error(
        StatusCode::FailedPrecondition,
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
    return Status::error(u.code(),
                         ("rerunHead: undoing the previous run failed, and it was "
                          "dropped: " +
                          std::string(u.message()))
                             .c_str());
  }

  Result<PropertyBag> out = [&] {
    PrefixScope prefix(ctx, old->prefix());
    return run(ctx, def, std::move(fresh).value());
  }();
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

Status
CommandExecutor::revertHead(Context &ctx,
                            litestl::util::function_ref<bool(CommandRecord &rec)> which)
{
  CDCC_TRY(rt_.busyIfEvaluating("revertHead"));
  CDCC_TRY(refuseNested("revertHead"));
  if (open_) {
    return busy("revertHead");
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  while (CommandRecord *head = history_ ? history_->head() : nullptr) {
    if (!which(*head)) {
      break;
    }
    RecordPtr rec = history_->popHead();
    {
      SlotScope slot(slotDepth_);
      rec->undo(ctx);
    }
    discardRecord(*rec);
    CDCC_TRY(collect(Status()));
  }
  return Status();
}

Status
CommandExecutor::beginInteractive(Context &ctx, const CommandDef &def, PropertyBag inputs)
{
  CDCC_TRY(rt_.busyIfEvaluating("beginInteractive"));
  CDCC_TRY(refuseNested("beginInteractive"));
  if (open_) {
    return busy(def.name().c_str());
  }
  if (!def.has(CommandFlags::Interactive)) {
    return Status::error(
        StatusCode::FailedPrecondition,
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
  interactiveSerial_++;
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

const CommandDef *CommandExecutor::interactiveDef() const
{
  return open_ ? &open_->def() : nullptr;
}

Status CommandExecutor::updateInteractive(
    Context &ctx, litestl::util::function_ref<Status(Context &ctx, Command &cmd)> fn)
{
  CDCC_TRY(rt_.busyIfEvaluating("updateInteractive"));
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition,
                         "updateInteractive inside a command");
  }
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition,
                         "updateInteractive: no interactive command");
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  SlotScope slot(slotDepth_);
  BagScope scope(ctx, *open_);
  return fn(ctx, open_->command());
}

Status CommandExecutor::commit(Context &ctx)
{
  CDCC_TRY(rt_.busyIfEvaluating("commit"));
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition, "commit inside a command");
  }
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition,
                         "commit: no interactive command");
  }
  if (rt_.hasStickyError()) {
    /* The live phase failed: what it left applied is unknown, so undo it all. */
    Status live = rt_.takeStickyError();
    Status c = cancel(ctx);
    Status s = Status::error(live.code(),
                             ("commit: the live edit failed, so the command "
                              "was cancelled: " +
                              std::string(live.message()))
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
  interactiveSerial_++;
  Result<PropertyBag> out = finish(ctx, std::move(rec));
  return out.ok() ? Status() : out.status();
}

Status CommandExecutor::cancel(Context &ctx)
{
  CDCC_TRY(rt_.busyIfEvaluating("cancel"));
  if (running() || slotDepth_ > 0) {
    return Status::error(StatusCode::FailedPrecondition, "cancel inside a command");
  }
  if (!open_) {
    return Status::error(StatusCode::FailedPrecondition,
                         "cancel: no interactive command");
  }
  Binding bind(ctx, this);
  CDCC_TRY(bind.status());
  RecordPtr rec = std::move(open_);
  interactiveSerial_++;
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

Result<PropertyBag>
inputsFromInvocation(Context &ctx, const CommandDef &def, const Invocation &inv)
{
  Runtime &rt = ctx.runtime();
  PropertyBag bag = def.newInputs();
  rt.ext<LastUsedStore>().apply(def, bag);
  /* Those that aren't dynamic first, since the others may depend on them. */
  for (bool dynamic : {false, true}) {
    for (const InvocationArg &arg : inv.args) {
      const int i = bag.indexOf(arg.name.c_str());
      if (i < 0) {
        return detail::unknownInput(def, arg);
      }
      if (bag.def(i).isDynamic() != dynamic) {
        continue;
      }
      const PropertyDef in = bag.resolvedDef(i);
      Result<PropValue> v = in.kind() == PropKind::Pointer
                                ? parsePointer(ctx, in, arg.value)
                                : parseLiteral(rt, in, arg.value);
      if (!v.ok()) {
        return v.status();
      }
      CDCC_TRY(bag.setAt(i, *v));
    }
  }
  return bag;
}

/* What `ctx`'s executor needs for `invocation`. */
static Result<PropertyBag>
bagFor(Context &ctx, const char *what, const Invocation &inv, const CommandDef *&def)
{
  if (!ctx.executor()) {
    return Status::error(StatusCode::FailedPrecondition,
                         (std::string(what) + ": the Context has no executor").c_str());
  }
  Runtime &rt = ctx.runtime();
  def = rt.ext<CommandRegistry>().find(inv.command.c_str());
  if (!def) {
    return rt.ext<CommandRegistry>().unknown(inv.command.c_str());
  }
  return inputsFromInvocation(ctx, *def, inv);
}

Result<PropertyBag> invoke(Context &ctx, std::string_view invocation)
{
  Result<Invocation> inv = parseInvocation(invocation);
  if (!inv.ok()) {
    return inv.status();
  }
  const CommandDef *def = nullptr;
  Result<PropertyBag> inputs = bagFor(ctx, "invoke", *inv, def);
  if (!inputs.ok()) {
    return inputs.status();
  }
  return ctx.executor()->run(ctx, *def, std::move(inputs).value());
}

Result<PropertyBag>
invokeAt(Context &ctx, const Path &prefix, std::string_view invocation)
{
  PrefixScope scope(ctx, prefix.size() > 0 ? &prefix : nullptr);
  return invoke(ctx, invocation);
}

Status canInvoke(Context &ctx, const Invocation &invocation)
{
  const CommandDef *def = nullptr;
  Result<PropertyBag> inputs = bagFor(ctx, "canInvoke", invocation, def);
  return inputs.ok() ? ctx.executor()->canRun(ctx, *def, std::move(inputs).value())
                     : inputs.status();
}

Status canInvoke(Context &ctx, std::string_view invocation)
{
  Result<Invocation> inv = parseInvocation(invocation);
  return inv.ok() ? canInvoke(ctx, *inv) : inv.status();
}

Status canInvokeAt(Context &ctx, const Path &prefix, std::string_view invocation)
{
  PrefixScope scope(ctx, prefix.size() > 0 ? &prefix : nullptr);
  return canInvoke(ctx, invocation);
}

} // namespace coredcc
