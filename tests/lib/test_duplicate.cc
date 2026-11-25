/* duplicateBlocks: both ways of copying, references, slots, attachments, and failures. */
#include "io_fixture.h"

#include "coredcc/lib/duplicate.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

BlockId cid(uint64_t n, ClientId client = ClientId::of(kCoredccClient))
{
  return BlockId{client, LocalId{7, n}};
}

Result<IdMap> duplicate(IoWorld &w, std::initializer_list<BlockHandle> list)
{
  const std::vector<BlockHandle> v = list;
  return duplicateBlocks(w.lib, std::span<const BlockHandle>(v.data(), v.size()));
}

Knows copying()
{
  Knows k;
  k.copyNotes = DuplicatePolicy::Copy;
  return k;
}

} // namespace

static void test_copies()
{
  for (TypeId type : {kDoc, kPlain}) {
    IoWorld w(copying());
    const BlockHandle a = w.doc(1, "a", type), b = w.doc(2, "b", type);
    const BlockHandle shared = w.doc(3, "shared"), gone = w.doc(4, "gone");
    w.link(a, b, shared);
    w.link(b, a, b);
    w.addOptEntry(a, w.id(b));
    w.addOptEntry(a, w.id(gone), "gone");
    test_assert(w.lib.setSlot(a, kDoc, 1, w.id(b), RefKind::OptStrong).ok());
    test_assert(w.lib.setSlot(a, kDoc, 2, w.id(shared), RefKind::Weak).ok());
    w.note(a, 5, w.id(b));
    test_assert(w.lib.attach(a, kBare, &w).ok()); /* dropped */
    w.root(a);
    w.lib.setFlag(b, BlockFlags::FakeUser, true);
    w.lib.pin(b);
    test_assert(w.lib.markDeleted(gone).ok());
    const int size = w.lib.size();
    const uint64_t epoch = w.lib.epoch();

    Result<IdMap> ids = duplicate(w, {a, b, a});
    test_assert(ids.ok() && ids->size() == 2 && w.lib.size() == size + 2);
    test_assert(w.docs.duplicated == (type == kDoc ? 2 : 0) && w.lib.epoch() > epoch);
    const BlockId na = ids->find(w.id(a)), nb = ids->find(w.id(b));
    const BlockHandle ca = w.lib.find(na), cb = w.lib.find(nb);
    test_assert(ca && cb && ca != a && cb != b);
    const Datablock &r = *w.lib.get(ca);
    test_assert(r.state() == BlockState::Live && r.type() == type);
    test_assert(strcmp(r.name().c_str(), "a") == 0 && r.flags() == BlockFlags::None);
    test_assert(w.lib.get(cb)->flags() == BlockFlags::FakeUser && w.lib.get(cb)->pins() == 0);
    const Doc &d = *w.data(ca);
    test_assert(d.number == 10 && w.data(ca) != w.data(a));
    test_assert(d.mesh.target == nb && w.lib.resolveRef(d.mesh) == cb);
    test_assert(d.parent.target == w.id(shared)); /* not in the set */
    test_assert(w.data(cb)->mesh.target == na && w.data(cb)->parent.target == nb);
    test_assert(d.entries[0].refs[0].target == nb && d.entries[1].refs[0].target == w.id(gone));
    if (type == kPlain) {
      test_assert(d.entries[0].value.tryPointer()->owner.block == nb);
    }
    test_assert(w.lib.slot(ca, kDoc, 1)->target == nb);
    test_assert(w.lib.slot(ca, kDoc, 1)->kind == RefKind::OptStrong);
    test_assert(w.lib.slot(ca, kDoc, 2)->target == w.id(shared));
    const Note *n = static_cast<const Note *>(w.lib.attachment(ca, kNote));
    test_assert(n && n != w.lib.attachment(a, kNote) && n->value == 5 && n->about.target == nb);
    test_assert(!w.lib.attachment(ca, kBare));
    /* The sources are as they were. */
    test_assert(w.data(a)->mesh.target == w.id(b) && w.lib.slot(a, kDoc, 1)->target == w.id(b));
    test_assert(static_cast<Note *>(w.lib.attachment(a, kNote))->about.target == w.id(b));
    test_assert(w.lib.usersOf(cb).size() == 2 && w.valid());
    w.lib.unpin(b);
    test_assert(w.lib.detach(a, kBare).ok());
  }
}

static void test_unread()
{
  /* Placeholders and opaque attachments are copied as they are, references followed. */
  Bytes bytes;
  {
    IoWorld from;
    const BlockHandle a = from.doc(1, "a"), b = from.doc(2, "b");
    from.root(a);
    from.link(a, b);
    from.note(a, 5, from.id(b));
    bytes = from.save();
  }
  Knows k;
  k.doc = k.note = false;
  IoWorld w(k);
  test_assert(w.load(bytes).ok());
  const BlockHandle a = w.at(1), b = w.at(2);
  Result<IdMap> ids = duplicate(w, {a, b});
  test_assert(ids.ok());
  const BlockHandle ca = w.lib.find(ids->find(w.id(a)));
  const BlockId nb = ids->find(w.id(b));
  const Datablock &r = *w.lib.get(ca);
  test_assert(r.state() == BlockState::Placeholder && r.placeholderReason() == PlaceholderReason::MissingType);
  test_assert(sameBytes(r.placeholderPayload(), w.lib.get(a)->placeholderPayload()));
  int toB = 0;
  test_assert(w.lib.foreachRef(ca, [&](RefCell &c) {
    toB += c.target == nb ? 1 : 0;
    test_assert(c.target != w.id(b));
  }).ok());
  test_assert(toB == 2);
  registerDoc(w.rt, w.docs);
  registerNote(w.rt, w.notes);
  test_assert(detail::resolvePlaceholders(w.lib).ok());
  test_assert(w.data(ca)->mesh.target == nb);
  test_assert(static_cast<Note *>(w.lib.attachment(ca, kNote))->about.target == nb);
}

static void test_refused()
{
  IoWorld w(copying());
  const BlockHandle a = w.doc(1, "a"), p = w.doc(2, "p", kPlain);
  const auto unchanged = [&](Result<IdMap> r, StatusCode code, const char *what) {
    if (r.ok()) {
      fprintf(stderr, "no error, wanted %s\n", what);
      return false;
    }
    return r.status().code() == code && hasMsg(r.status(), what) && w.lib.size() == 2 && w.valid();
  };
  test_assert(unchanged(duplicate(w, {a, BlockHandle()}), StatusCode::NotFound, "no such block"));
  w.docs.fault = DocFault::Error;
  test_assert(unchanged(duplicate(w, {a}), StatusCode::Unsupported, "duplicating 'a': no copies"));
  test_assert(unchanged(duplicate(w, {p}), StatusCode::Unsupported, "duplicating 'p': not today"));
  w.docs.fault = DocFault::Sticky;
  test_assert(unchanged(duplicate(w, {a}), StatusCode::Internal, "duplicate threw"));
  test_assert(unchanged(duplicate(w, {p}), StatusCode::Internal, "serialize threw"));
  w.docs.fault = DocFault::NoData;
  test_assert(unchanged(duplicate(w, {a}), StatusCode::Internal, "without calling setData"));
  test_assert(unchanged(duplicate(w, {p}), StatusCode::Internal, "without calling setData"));
  w.docs.fault = DocFault::LeaveBytes;
  test_assert(unchanged(duplicate(w, {p}), StatusCode::InvalidArgument, "left over"));
  /* A later failure destroys what came before it. */
  const int destroyed = w.docs.destroyed;
  test_assert(unchanged(duplicate(w, {a, p}), StatusCode::InvalidArgument, "duplicating 'p'"));
  test_assert(w.docs.destroyed == destroyed + 2); /* p's data in the read, and the copy of a */
  w.docs.fault = DocFault::None;

  Note *n = w.note(a, 1, w.id(p));
  for (DocFault fault : {DocFault::NoData, DocFault::Sticky}) {
    w.notes.fault = fault;
    test_assert(unchanged(duplicate(w, {a}), StatusCode::Internal,
                          fault == DocFault::NoData ? "copy gave no data" : "copy threw"));
    test_assert(w.notes.alive == 1);
  }
  w.notes.fault = DocFault::None;
  w.notes.failForeach = true;
  test_assert(unchanged(duplicate(w, {a}), StatusCode::Internal, "note foreachRef threw"));
  w.notes.failForeach = false;
  test_assert(n->about.target == w.id(p) && w.notes.alive == 1);

  /* The library is busy to duplicate and to copy; a repeated GUID fails. */
  bool busy = false;
  w.docs.onDestroy = nullptr;
  w.notes.onCopy = [&]() { busy = w.lib.create(kDoc, "x", nullptr).status().code() == StatusCode::Busy; };
  w.docs.onDuplicate = [&]() {
    test_assert(duplicate(w, {a}).status().code() == StatusCode::Busy);
  };
  test_assert(duplicate(w, {a}).ok() && busy);
  w.notes.onCopy = nullptr;
  w.docs.onDuplicate = nullptr;
  test_assert(*w.lib.collect() >= 0);

  IoWorld w2;
  const BlockHandle h = w2.doc(1, "x");
  w2.rt.setGuidSource([]() { return LocalId{7, 1}; });
  test_assert(duplicate(w2, {h}).status().code() == StatusCode::Internal && w2.lib.size() == 1);
  w2.rt.setGuidSource({});
  {
    EvaluationScope scope(w2.rt);
    test_assert(duplicate(w2, {h}).status().code() == StatusCode::Busy);
  }
  w2.rt.setStickyError(Status::error(StatusCode::Internal, "before"));
  test_assert(duplicate(w2, {h}).status().code() == StatusCode::FailedPrecondition);
  w2.rt.takeStickyError().ignore();

  /* Types with neither way, and blocks without data. */
  const BlockHandle t = makeThing(w2.lib, "thing");
  test_assert(duplicate(w2, {t}).status().code() == StatusCode::Unsupported);
  test_assert(w2.lib.markDeleted(h).ok());
  test_assert(hasMsg(duplicate(w2, {h}).status(), "it is InHistory, without"));
  test_assert(w2.lib.restoreFromHistory(h).ok() && w2.lib.onHostDestroyed(w2.id(h)).ok());
  test_assert(hasMsg(duplicate(w2, {h}).status(), "it is Detached"));
  PlaceholderInfo info;
  info.reason = PlaceholderReason::MissingBlock;
  Result<BlockHandle> m = w2.lib.addPlaceholder(cid(50), std::move(info));
  test_assert(m.ok() && hasMsg(duplicate(w2, {*m}).status(), "it is Placeholder"));
  const std::vector<BlockHandle> none;
  Result<IdMap> nothing = duplicateBlocks(w2.lib, std::span<const BlockHandle>(none.data(), 0));
  test_assert(nothing.ok() && nothing->size() == 0);
}

/* The sources' placeholder type or client may be one nobody registered. */
static void test_unknown_client()
{
  IoWorld w;
  PlaceholderInfo info;
  info.typeName = "x.Lost";
  info.name = "lost";
  info.sourceName = "file";
  const ClientId ghost = ClientId::of("ghost");
  Result<BlockHandle> h = w.lib.addPlaceholder(cid(1, ghost), std::move(info));
  Result<IdMap> ids = duplicate(w, {*h});
  test_assert(ids.ok());
  const Datablock &c = *w.lib.get(w.lib.find(ids->find(cid(1, ghost))));
  test_assert(c.id().client == ghost && strcmp(c.sourceName().c_str(), "file") == 0);
  test_assert(strcmp(c.name().c_str(), "lost") == 0);
}

int main()
{
  test_copies();
  test_unread();
  test_refused();
  test_unknown_client();
  return test_end();
}
