/* TypeAliases in the library section: a renamed block type, attachment type and slot
 * namespace load from a file that has the old names, and save the current ones; a block
 * that has a slot or an attachment by both names is refused, and so is an alias whose old
 * name is still registered. */
#include "io_fixture.h"

#include "coredcc/lib/type_names.h"

#include "litestl/tests/test_util.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;

namespace {

const TypeId kDoc2 = TypeId::of("test.Doc2");
const TypeId kNote2 = TypeId::of("test.Note2");

void must(const Status &s)
{
  if (!s.ok()) {
    fprintf(stderr, "%s\n", s.message());
  }
  test_assert(s.ok());
}

/* A world that has the current names only: test.Doc2 and test.Note2. */
struct Renamed {
  NoteHost notes;
  IoWorld w{Knows{.doc = false, .note = false}};
  explicit Renamed(bool aliases = true)
  {
    registerDoc(w.rt, w.docs, 3, "test.Doc2");
    registerNote(w.rt, notes, 2, DuplicatePolicy::Drop, "test.Note2");
    if (aliases) {
      TypeAliases &a = w.rt.ext<TypeAliases>();
      must(a.add(TypeNameKind::Block, "test.Doc", "test.Doc2"));
      must(a.add(TypeNameKind::Attachment, "test.Note", "test.Note2"));
      must(a.add(TypeNameKind::Slot, "test.Note", "test.Note2"));
    }
  }
};

/* A saved doc, with a note and a slot under test.Note, that refers to another doc. */
Bytes oldFile()
{
  IoWorld w;
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b");
  w.root(a);
  w.root(b);
  w.note(a, 42, w.id(b));
  must(w.lib.setSlot(a, kNote, 5, w.id(b), RefKind::Weak));
  return w.save();
}

void checkRenamed(IoWorld &w)
{
  const BlockHandle a = w.at(1), b = w.at(2);
  test_assert(a && b && w.lib.get(a)->type() == kDoc2 && w.lib.get(b)->type() == kDoc2);
  const Note *n = static_cast<const Note *>(w.lib.attachment(a, kNote2));
  test_assert(n && n->value == 42 && n->about.target == w.id(b));
  test_assert(!w.lib.attachment(a, kNote));
  const RefCell *slot = w.lib.slot(a, kNote2, 5);
  test_assert(slot && slot->target == w.id(b) && !w.lib.slot(a, kNote, 5));
}

void testLoadAndSave()
{
  const Bytes old = oldFile();
  Renamed r;
  must(r.w.load(old));
  checkRenamed(r.w);
  /* Saved under the current names: a world without the aliases reads it. */
  Renamed plain(false);
  must(plain.w.load(r.w.save()));
  checkRenamed(plain.w);
}

void testAppend()
{
  const Bytes old = oldFile();
  Renamed r;
  Result<AppendResult> appended = r.w.append(old);
  must(appended.status());
  int docs = 0;
  r.w.lib.forEach([&](BlockHandle h, const Datablock &block) {
    docs += block.type() == kDoc2;
    if (block.type() == kDoc2 && r.w.lib.attachment(h, kNote2)) {
      test_assert(static_cast<const Note *>(r.w.lib.attachment(h, kNote2))->value == 42);
    }
  });
  test_assert(docs == 2);
}

/* A block with a note under each name: one of them is an old name. */
void testAttachmentTwice()
{
  NoteHost second;
  Bytes file;
  {
    IoWorld w;
    registerNote(w.rt, second, 2, DuplicatePolicy::Drop, "test.Note2");
    const BlockHandle a = w.doc(1, "a");
    w.root(a);
    w.note(a, 1, BlockId());
    Note *n = litestl::alloc::New<Note>("Note");
    second.alive++;
    must(w.lib.attach(a, kNote2, n));
    file = w.save();
  }
  Renamed r;
  const Status s = r.w.load(file);
  test_assert(s.code() == StatusCode::InvalidArgument &&
              has(s.message(), "one attachment twice"));
  test_assert(r.w.lib.size() == 0);
}

void testSlotTwice()
{
  NoteHost second;
  Bytes file;
  {
    IoWorld w;
    registerNote(w.rt, second, 2, DuplicatePolicy::Drop, "test.Note2");
    const BlockHandle a = w.doc(1, "a");
    w.root(a);
    must(w.lib.setSlot(a, kNote, 3, BlockId(), RefKind::OptStrong));
    must(w.lib.setSlot(a, kNote2, 3, BlockId(), RefKind::OptStrong));
    file = w.save();
  }
  Renamed r;
  const Status s = r.w.load(file);
  test_assert(s.code() == StatusCode::InvalidArgument &&
              has(s.message(), "one slot twice"));
  /* The same key under two names that aren't aliases is fine. */
  Renamed plain(false);
  must(plain.w.load(file));
}

/* An alias whose old name is still registered would turn every stored use of that type
 * into the current one. */
void testOldNameRegistered()
{
  const Bytes old = oldFile();
  IoWorld w;
  registerDoc(w.rt, w.docs, 3, "test.Doc2");
  must(w.rt.ext<TypeAliases>().add(TypeNameKind::Block, "test.Doc", "test.Doc2"));
  const Status s = w.load(old);
  test_assert(s.code() == StatusCode::FailedPrecondition &&
              has(s.message(), "still a registered type"));
  test_assert(w.append(old).status().code() == StatusCode::FailedPrecondition);
  test_assert(w.lib.size() == 0);
}

/* The current name's id is checked against the names the Runtime knows, as a stored
 * name's is. */
void testCurrentNameClash()
{
  const Bytes old = oldFile();
  IoWorld w{Knows{.doc = false}};
  must(w.rt.ext<TypeAliases>().add(TypeNameKind::Block, "test.Doc", "test.Fresh"));
  test_assert(w.rt.debugRegisterNameAs(
                      IdDomain::Type, TypeId::of("test.Fresh").value, "test.Other")
                  .ok());
  const Status s = w.load(old);
  test_assert(s.code() == StatusCode::InvalidArgument && has(s.message(), "test.Fresh") &&
              has(s.message(), "test.Other"));
}

} // namespace

int main()
{
  testLoadAndSave();
  testAppend();
  testAttachmentTwice();
  testSlotTwice();
  testOldNameRegistered();
  testCurrentNameClash();
  return test_end();
}
