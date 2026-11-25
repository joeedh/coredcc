/* Several Libraries with one executor: a command that was done in one is refused through another
 * one's Context, and when its own is gone (finding 16 of the review of library IO). */
#include "command_fixture.h"

#include "coredcc/api/ref_text.h"
#include "coredcc/command/attachment_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/history_block.h"
#include "coredcc/command/library_commands.h"
#include "coredcc/command/snapshot_undo.h"
#include "coredcc/lib/attachment.h"
#include "coredcc/lib/io/library_io.h"

test_init;

using litestl::util::Vector;

namespace {

BlockId idOf(uint64_t n)
{
  return BlockId{ClientId::of(kCoredccClient), LocalId{7, n}};
}

PropValue pointer(BlockId id)
{
  StableRef r;
  r.owner = OwnerKey::ofBlock(id);
  return PropValue::ofPointer(r);
}

/* Data that says which library it is for. */
struct Tag {
  int *alive = nullptr;
};
struct TagProvider {
  int *alive = nullptr;
  void destroy(void *data)
  {
    (*alive)--;
    litestl::alloc::Delete(static_cast<Tag *>(data));
  }
  void *create(const Datablock &)
  {
    (*alive)++;
    return litestl::alloc::New<Tag>("Tag");
  }
  static lbt::Struct<TagProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<TagProvider>("test::TagProvider", sizeof(TagProvider));
    BIND_STRUCT_METHOD(st, destroy, MARGS("data"));
    BIND_STRUCT_METHOD(st, create, MARGS("block"));
    return st;
  }
};
const TypeId kTag = TypeId::of("test.Tag");

/* A block type that can be copied: an integer. */
struct NumberProvider {
  void *data(const Datablock &b) const
  {
    return b.clientData();
  }
  void foreachRef(const Datablock &, RefVisitor &) const {}
  void destroy(const Datablock &b)
  {
    litestl::alloc::Delete(static_cast<int *>(b.clientData()));
  }
  Status serialize(const Datablock &b, BlockWriter &out) const
  {
    out.bytes().u32(uint32_t(*static_cast<int *>(b.clientData())));
    return Status();
  }
  Status deserialize(const Datablock &, BlockReader &in)
  {
    in.setData(litestl::alloc::New<int>("number", int(in.bytes().u32())));
    return Status();
  }
  static lbt::Struct<NumberProvider> *defineBindings()
  {
    auto *st = new lbt::Struct<NumberProvider>("test::NumberProvider", sizeof(NumberProvider));
    BIND_STRUCT_METHOD(st, data, MARGS("block"));
    BIND_STRUCT_METHOD(st, foreachRef, MARGS("block", "visitor"));
    BIND_STRUCT_METHOD(st, destroy, MARGS("block"));
    BIND_STRUCT_METHOD(st, serialize, MARGS("block", "out"));
    BIND_STRUCT_METHOD(st, deserialize, MARGS("block", "in"));
    return st;
  }
};
const TypeId kNumber = TypeId::of("test.Number");

/* A snapshot of one number. */
int g_state = 1;
struct NumberSnapshot {
  Status capture(Context &, Vector<uint8_t> &blob) const
  {
    blob.append(uint8_t(g_state));
    return Status();
  }
  Status restore(Context &, const Vector<uint8_t> &blob)
  {
    g_state = blob[0];
    return Status();
  }
  static lbt::Struct<NumberSnapshot> *defineBindings()
  {
    auto *st = new lbt::Struct<NumberSnapshot>("test::NumberSnapshot", sizeof(NumberSnapshot));
    BIND_STRUCT_METHOD(st, capture, MARGS("ctx", "blob"));
    BIND_STRUCT_METHOD(st, restore, MARGS("ctx", "blob"));
    return st;
  }
};
struct SetState {
  SnapshotUndo undo_{"number"};
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    CDCC_TRY(undo_.begin(ctx));
    g_state = 50;
    return undo_.end(ctx);
  }
  void undo(Context &ctx)
  {
    undo_.undo(ctx);
  }
  Status redo(Context &ctx)
  {
    return undo_.redo(ctx);
  }
  uint64_t memSize() const
  {
    return undo_.memSize();
  }
  void discard()
  {
    undo_.discard();
  }
  static lbt::Struct<SetState> *defineBindings()
  {
    auto *st = new lbt::Struct<SetState>("test::SetState", sizeof(SetState));
    BIND_COMMAND_CORE(st);
    BIND_STRUCT_METHOD(st, redo, MARGS("ctx"));
    return st;
  }
};

/* Two libraries of one Runtime, whose blocks have the same ids. */
struct Documents {
  Setup s;
  int tags = 0;
  Library *one = nullptr;
  Library two{&s.w.rt};
  Context in1, in2;

  Documents()
      : one(litestl::alloc::New<Library>("Library", &s.w.rt)), in1(&s.w.rt, one, s.ctx.root()),
        in2(&s.w.rt, &two, s.ctx.root())
  {
    Runtime &rt = s.w.rt;
    in1.setExecutor(&s.ex);
    in2.setExecutor(&s.ex);
    test_assert((registerImpl<AttachmentProvider, TagProvider>(rt, "test.Tag#p").ok()));
    TagProvider *p = litestl::alloc::New<TagProvider>("provider");
    p->alive = &tags;
    AttachmentTypeInfo tag;
    tag.name = "test.Tag";
    tag.storage = BlockStorage::Transient;
    test_assert(rt.ext<AttachmentTypeRegistry>()
                    .add(tag, std::move(own<AttachmentProvider>(rt, "test.Tag#p", p)).value())
                    .ok());
    test_assert((registerImpl<BlockProvider, NumberProvider>(rt, "test.Number#p").ok()));
    BlockTypeInfo number;
    number.name = "test.Number";
    number.client = ClientId::of(kCoredccClient);
    test_assert(rt.ext<BlockTypeRegistry>()
                    .add(number, std::move(own<BlockProvider>(
                                               rt, "test.Number#p",
                                               litestl::alloc::New<NumberProvider>("provider")))
                                     .value())
                    .ok());
    test_assert((registerImpl<SnapshotProvider, NumberSnapshot>(rt, "test.Snapshot").ok()));
    SnapshotRegistry &snapshots = rt.ext<SnapshotRegistry>();
    test_assert(snapshots
                    .add("number", std::move(own<SnapshotProvider>(
                                                 rt, "test.Snapshot",
                                                 litestl::alloc::New<NumberSnapshot>("provider")))
                                       .value())
                    .ok());
    test_assert(snapshots.validate().ok());
    test_assert(addNativeCommand<SetState>(rt, CommandDef("test.state")).ok());
    test_assert(registerBlockCommands(rt).ok());
    fill(*one);
    fill(two);
    g_state = 1;
  }
  ~Documents()
  {
    s.history.clear();
    drop();
  }
  void drop()
  {
    if (one) {
      litestl::alloc::Delete(one);
      one = nullptr;
    }
  }
  static void fill(Library &lib)
  {
    for (uint64_t n = 1; n <= 2; n++) {
      test_assert(lib.createWithId(kNumber, idOf(n), "number", litestl::alloc::New<int>("n", int(n)))
                      .ok());
      lib.setFlag(lib.find(idOf(n)), BlockFlags::Root, true);
    }
    PlaceholderInfo info;
    info.typeName = "x.Gone";
    RefCell row(RefKind::OptStrong);
    row.target = idOf(1);
    info.payload.refs.append(row);
    test_assert(lib.addPlaceholder(idOf(3), info).ok());
    test_assert(lib.attach(lib.find(idOf(2)), kTag, litestl::alloc::New<Tag>("Tag")).ok());
  }
  /* What a library says, with the parts a file doesn't have. */
  static std::string text(const Library &lib)
  {
    std::string out;
    lib.forEach([&](BlockHandle h, const Datablock &b) {
      if (b.state() == BlockState::InHistory) {
        return;
      }
      out += std::string(b.name().c_str()) + " " + std::to_string(int(b.state())) +
             (lib.attachment(h, kTag) ? " tag" : "") + (lib.data(h) ? " data" : "");
      if (const Vector<RefCell> *rows = lib.refTable(h)) {
        out += (*rows)[0].isNull() ? " null" : " row";
      }
      out += ";";
    });
    return out;
  }
  Status run(Context &ctx,
             const char *command,
             std::initializer_list<std::pair<const char *, PropValue>> inputs)
  {
    PropertyBag in = s.w.rt.ext<CommandRegistry>().find(command)->newInputs();
    for (const auto &i : inputs) {
      test_assert(in.set(i.first, i.second).ok());
    }
    return s.ex.run(ctx, command, std::move(in)).status();
  }
};

using Do = litestl::util::function<Status(Documents &)>;

struct Case {
  const char *name;
  Do run;
};

PropValue block(uint64_t n)
{
  return pointer(idOf(n));
}

std::vector<Case> cases()
{
  return {
      {"rename",
       [](Documents &d) {
         return d.run(d.in1, kRenameBlockCommand,
                      {{"block", block(1)}, {"name", PropValue::ofString("renamed")}});
       }},
      {"unlink",
       [](Documents &d) {
         return d.run(d.in1, kUnlinkRefCommand,
                      {{"block", block(3)}, {"index", PropValue::ofInt(0)}});
       }},
      {"attach",
       [](Documents &d) {
         return d.run(d.in1, kAttachCommand,
                      {{"block", block(1)}, {"type", PropValue::ofString("test.Tag")}});
       }},
      {"detach",
       [](Documents &d) {
         return d.run(d.in1, kDetachCommand,
                      {{"block", block(2)}, {"type", PropValue::ofString("test.Tag")}});
       }},
      {"delete",
       [](Documents &d) { return d.run(d.in1, kDeleteBlockCommand, {{"block", block(1)}}); }},
      {"duplicate",
       [](Documents &d) { return d.run(d.in1, kDuplicateBlockCommand, {{"block", block(1)}}); }},
      {"append",
       [](Documents &d) {
         Runtime &rt = d.s.w.rt;
         Library from(&rt);
         test_assert(from.create(kNumber, "n", litestl::alloc::New<int>("n", 1)).ok());
         from.forEach([&](BlockHandle h, const Datablock &) {
           from.setFlag(h, BlockFlags::Root, true);
         });
         Result<Vector<uint8_t>> file = saveLibrary(from);
         return appendUndo(d.in1, std::span<const uint8_t>(file->data(), size_t(file->size())))
             .status();
       }},
      {"property",
       [](Documents &d) {
         return setValueUndo(d.in1, "tool.strength", PropValue::ofFloat(0.75));
       }},
      {"snapshot", [](Documents &d) { return d.run(d.in1, "test.state", {}); }},
  };
}

double strength(Documents &d)
{
  return getValue(d.in2, "tool.strength")->asFloat();
}

} // namespace

/* Through the other document's Context. */
static void test_other()
{
  for (const Case &c : cases()) {
    for (bool redo : {false, true}) {
      Documents d;
      const std::string before = Documents::text(*d.one), other = Documents::text(d.two);
      Status ran = c.run(d);
      test_assert(ran.ok());
      const std::string after = Documents::text(*d.one);
      const double value = strength(d);
      const int state = g_state;
      if (redo) {
        test_assert(d.s.ex.undo(d.in1).ok() && Documents::text(*d.one) == before);
      }
      const double undone = strength(d);
      const int undoneState = g_state;
      const uint64_t epoch = d.two.epoch();
      Status s = redo ? d.s.ex.redo(d.in2) : d.s.ex.undo(d.in2);
      if (s.code() != StatusCode::FailedPrecondition || Documents::text(d.two) != other) {
        fprintf(stderr, "%s, redo %d: %s\n", c.name, int(redo), s.message());
      }
      test_assert(s.code() == StatusCode::FailedPrecondition && hasMsg(s, "in another Library"));
      test_assert(Documents::text(d.two) == other && d.two.epoch() == epoch);
      test_assert(Documents::text(*d.one) == (redo ? before : after));
      test_assert(strength(d) == (redo ? undone : value) &&
                  g_state == (redo ? undoneState : state));
      test_assert(d.two.validate().ok() && d.one->validate().ok());
    }
  }
}

/* Its own Library is gone: an error, and nothing dangles. */
static void test_gone()
{
  for (const Case &c : cases()) {
    for (int when = 0; when < 3; when++) {
      Documents d;
      test_assert(c.run(d).ok());
      if (when == 1) {
        test_assert(d.s.ex.undo(d.in1).ok());
      }
      d.drop();
      Context gone(&d.s.w.rt, nullptr, d.s.ctx.root());
      gone.setExecutor(&d.s.ex);
      if (when < 2) {
        Status s = when ? d.s.ex.redo(gone) : d.s.ex.undo(gone);
        if (s.code() != StatusCode::FailedPrecondition) {
          fprintf(stderr, "%s, %d: %s\n", c.name, when, s.message());
        }
        test_assert(s.code() == StatusCode::FailedPrecondition && hasMsg(s, "Library is gone"));
      }
      test_assert(d.s.history.validate().ok() && !d.s.w.rt.hasStickyError());
      d.s.history.clear();
      /* What was detached is freed by its provider all the same. */
      test_assert(!d.s.w.rt.hasStickyError());
    }
  }
}

static void test_helper()
{
  Documents d;
  HistoryBlock b(*d.one, d.one->find(idOf(1)));
  test_assert(*detail::libraryOf(d.in1, b.library()) == d.one);
  test_assert(detail::libraryOf(d.in2, d.one->ref()).status().code() ==
              StatusCode::FailedPrecondition);
  test_assert(hasMsg(detail::libraryOf(d.in1, LibraryRef()).status(), "is gone"));
  /* Folding a gesture moves its pins. */
  test_assert(setValueUndo(d.in1, "tool.strength", PropValue::ofFloat(0.1), 5).ok());
  test_assert(setValueUndo(d.in1, "tool.strength", PropValue::ofFloat(0.2), 5).ok());
  test_assert(d.s.history.size() == 1 && d.s.ex.undo(d.in1).ok());
}

int main()
{
  test_other();
  test_gone();
  test_helper();
  return test_end();
}
