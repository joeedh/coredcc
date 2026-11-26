/* Saving and loading for scripts: bytes, reports, limits, and where they are refused. */
#include "coredcc/bind/foreign_command.h"
#include "coredcc/bind/library_facades.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/lib/pin.h"

#include "../lib/io_fixture.h"
#include "fake_host.h"

test_init;

using namespace coredcc;
using namespace coredcc_test;
namespace sc = coredcc::script;

namespace {

struct BindWorld {
  IoWorld w;
  LinearHistory history;
  CommandExecutor ex{&w.rt, &history};
  Context ctx{&w.rt, &w.lib, PointerRef()};

  explicit BindWorld(Knows k = {}) : w(k)
  {
    ctx.setExecutor(&ex);
    test_assert(registerBlockCommands(w.rt).ok());
  }
  ~BindWorld()
  {
    history.clear();
  }
};

/* a, b and, by a strong ref, a block that isn't in the file. */
void fill(IoWorld &w)
{
  const BlockHandle a = w.doc(1, "a"), b = w.doc(2, "b \"quoted\"");
  w.root(a);
  w.link(a, b);
  w.data(b)->mesh.target = BlockId{kAcme, LocalId{7, 50}};
  w.lib.noteRefsChanged();
}

Bytes sample()
{
  IoWorld w;
  fill(w);
  return w.save();
}

/* The JSON has `want`. */
bool shows(const litestl::util::string &json, const char *want)
{
  const bool found = strstr(json.c_str(), want) != nullptr;
  if (!found) {
    fprintf(stderr, "no %s\nin %s\n", want, json.c_str());
  }
  return found;
}

const char *const kA = "coredcc,00000000000000070000000000000001";

} // namespace

static void test_bytes()
{
  const sc::ScriptBytes none;
  test_assert(sc::bytesSize(none) == 0 && sc::bytesRead(none, 0, {}).ok());
  Bytes b;
  for (uint8_t i = 0; i < 10; i++) {
    b.append(i);
  }
  const sc::ScriptBytes bytes{Bytes(b)};
  const sc::ScriptBytes copy = bytes;
  test_assert(sc::bytesSize(copy) == 10 && copy.bytes().data() == bytes.bytes().data());
  uint8_t out[10] = {};
  test_assert(sc::bytesRead(bytes, 7, std::span(out, 3)).ok() && out[0] == 7 &&
              out[2] == 9 && out[3] == 0);
  test_assert(sc::bytesRead(bytes, 0, out).ok() && memcmp(out, b.data(), 10) == 0);
  test_assert(sc::bytesRead(bytes, 10, {}).ok());
  for (int64_t offset : {int64_t(-1), int64_t(8), int64_t(11), INT64_MAX, INT64_MIN}) {
    test_assert(sc::bytesRead(bytes, offset, std::span(out, 3)).code() ==
                StatusCode::OutOfRange);
  }
}

static void test_save()
{
  BindWorld c;
  fill(c.w);
  const BlockHandle pinned = c.w.doc(3, "pinned");
  const LibraryPin pin(c.w.lib, pinned);
  sc::SaveResult r = sc::saveLibrary(c.ctx);
  test_assert(r.status.ok() && sc::bytesSize(r.value) == int64_t(c.w.save().size()));
  SaveReport report;
  c.w.save(&report);
  test_assert(shows(r.report, "\"blocks\":3,"));
  test_assert(
      shows(r.report, ("\"loadBytes\":" + std::to_string(report.loadBytes)).c_str()));
  test_assert(
      shows(r.report, ("\"loadBlocks\":" + std::to_string(report.loadBlocks)).c_str()));
  test_assert(shows(r.report,
                    "\"hostPinned\":{\"total\":1,\"first\":[{\"block\":\"coredcc,"
                    "00000000000000070000000000000003\",\"name\":\"pinned\"}]}"));
  for (const char *key : {"transient", "throughTransient", "detached", "issues"}) {
    test_assert(shows(
        r.report, ("\"" + std::string(key) + "\":{\"total\":0,\"first\":[]}").c_str()));
  }
  c.w.docs.fault = DocFault::Error;
  r = sc::saveLibrary(c.ctx);
  test_assert(!r.status.ok() && sc::bytesSize(r.value) == 0 && r.report.size() == 0);
  c.w.docs.fault = DocFault::None;
  Context none(&c.w.rt, nullptr, PointerRef());
  test_assert(sc::saveLibrary(none).status.code() == StatusCode::FailedPrecondition);
}

static void test_load()
{
  const Bytes bytes = sample();
  {
    BindWorld c;
    sc::StringResult r = sc::loadLibrary(c.ctx, view(bytes), "file", 0, 0);
    test_assert(r.status.ok());
    test_assert(shows(r.value,
                      "{\"records\":2,\"missingType\":0,\"missingBlock\":1,"
                      "\"blocks\":{\"total\":3,\"first\":["));
    test_assert(shows(r.value,
                      "{\"block\":\"coredcc,00000000000000070000000000000002\","
                      "\"name\":\"b \\\"quoted\\\"\"}"));
    test_assert(shows(r.value, "{\"block\":\"acme,00000000000000070000000000000032\""));
    test_assert(strcmp(c.w.lib.get(c.w.at(50, kAcme))->sourceName().c_str(), "file") ==
                0);
    /* Not empty any more. */
    r = sc::loadLibrary(c.ctx, view(bytes), "", 0, 0);
    test_assert(r.status.code() == StatusCode::FailedPrecondition);
    sc::SaveResult saved = sc::saveLibrary(c.ctx);
    test_assert(saved.value.bytes().size() == bytes.size() &&
                memcmp(saved.value.bytes().data(), bytes.data(), bytes.size()) == 0);
  }
  /* Limits. */
  BindWorld c;
  const auto load = [&](int64_t maxBytes, int64_t maxBlocks) {
    return sc::loadLibrary(c.ctx, view(bytes), "", maxBytes, maxBlocks).status.code();
  };
  test_assert(load(10, 0) == StatusCode::OutOfRange &&
              load(0, 2) == StatusCode::OutOfRange);
  for (int64_t n : {int64_t(-1), INT64_MIN}) {
    test_assert(load(n, 0) == StatusCode::InvalidArgument &&
                load(0, n) == StatusCode::InvalidArgument);
  }
  test_assert(load(0, int64_t(1) << 32) == StatusCode::InvalidArgument);
  test_assert(c.w.lib.size() == 0);
  /* Damaged bytes, of any length. */
  for (size_t n = 0; n < size_t(bytes.size()); n++) {
    test_assert(!sc::loadLibrary(c.ctx, view(bytes).first(n), "", 0, 0).status.ok());
  }
  test_assert(c.w.lib.size() == 0 && load(int64_t(1) << 40, 3) == StatusCode::Ok);
  Context none(&c.w.rt, nullptr, PointerRef());
  test_assert(sc::loadLibrary(none, view(bytes), "", 0, 0).status.code() ==
              StatusCode::FailedPrecondition);
}

static void test_append()
{
  const Bytes bytes = sample();
  BindWorld c;
  sc::StringResult r = sc::appendLibrary(c.ctx, view(bytes), "file", 0, 0);
  test_assert(r.status.ok() && c.history.size() == 1 && c.w.lib.size() == 3);
  test_assert(shows(r.value,
                    ("{\"ids\":{\"total\":2,\"first\":[{\"from\":\"" + std::string(kA) +
                     "\",\"to\":\"coredcc,")
                        .c_str()));
  test_assert(shows(r.value, "\"records\":2,\"missingType\":0,\"missingBlock\":1,"));
  test_assert(!c.w.at(1)); /* a fresh id */
  test_assert(c.ex.undo(c.ctx).ok() && c.ex.redo(c.ctx).ok());
  for (const sc::StringResult &bad :
       {sc::appendLibrary(c.ctx, view(bytes), "", 10, 0),
        sc::appendLibrary(c.ctx, view(bytes), "", 0, 1),
        sc::appendLibrary(c.ctx, view(bytes), "", -1, 0),
        sc::appendLibrary(c.ctx, view(bytes).first(20), "", 0, 0)})
  {
    test_assert(!bad.status.ok() && bad.value.size() == 0);
  }
  test_assert(c.history.size() == 1);
  Context none(&c.w.rt, nullptr, PointerRef());
  test_assert(sc::appendLibrary(none, view(bytes), "", 0, 0).status.code() ==
              StatusCode::FailedPrecondition);
}

static void test_resolve()
{
  const Bytes bytes = sample();
  Knows k;
  k.doc = false;
  BindWorld c(k);
  sc::StringResult r = sc::loadLibrary(c.ctx, view(bytes), "", 0, 0);
  test_assert(r.status.ok() && shows(r.value, "\"records\":2,\"missingType\":2,"));
  test_assert(shows(r.value, "\"issues\":{\"total\":") &&
              shows(r.value,
                    ("{\"block\":\"" + std::string(kA) + "\",\"name\":\"a\",\"code\":\"")
                        .c_str()) &&
              shows(r.value, "\"message\":\"") && shows(r.value, "\"suggestions\":["));
  registerDoc(c.w.rt, c.w.docs);
  /* Not while there is something to undo, or to redo. */
  test_assert(sc::appendLibrary(c.ctx, view(bytes), "", 0, 0).status.ok());
  test_assert(c.history.canUndo());
  test_assert(sc::resolvePlaceholders(c.ctx).status.code() ==
              StatusCode::FailedPrecondition);
  test_assert(c.ex.undo(c.ctx).ok() && !c.history.canUndo() && c.history.canRedo());
  test_assert(sc::resolvePlaceholders(c.ctx).status.code() ==
              StatusCode::FailedPrecondition);
  test_assert(sc::clearHistory(c.ctx).ok() && c.history.size() == 0);
  r = sc::resolvePlaceholders(c.ctx);
  test_assert(r.status.ok() && shows(r.value, "{\"records\":0,\"missingType\":0,") &&
              shows(r.value, "\"blocks\":{\"total\":2,"));
  test_assert(c.w.lib.get(c.w.at(1))->state() == BlockState::Live);
  Context bare(&c.w.rt, &c.w.lib, PointerRef());
  test_assert(sc::resolvePlaceholders(bare).status.ok()); /* no history */
  test_assert(sc::clearHistory(bare).code() == StatusCode::FailedPrecondition);
}

/* A report doesn't grow with the file. */
static void test_large()
{
  BindWorld c;
  for (uint64_t i = 1; i <= 10005; i++) {
    c.w.root(c.w.doc(i, "n"));
  }
  sc::SaveResult saved = sc::saveLibrary(c.ctx);
  BindWorld to;
  sc::StringResult r = sc::appendLibrary(to.ctx, saved.value.bytes(), "", 0, 0);
  test_assert(r.status.ok() && shows(r.value, "\"ids\":{\"total\":10005,"));
  int ids = 0, blocks = 0;
  for (const char *p = r.value.c_str(); (p = strstr(p, "{\"from\":")); p++) {
    ids++;
  }
  for (const char *p = r.value.c_str(); (p = strstr(p, "{\"block\":")); p++) {
    blocks++;
  }
  test_assert(ids == 10000 && blocks == 10000);
}

/* From a script command's slots. */
static void test_slots()
{
  const Bytes bytes = sample();
  BindWorld c;
  fakehost::Host host;
  std::string log;
  const auto probe = [&](Context &ctx) {
    for (const StatusCode code :
         {sc::saveLibrary(ctx).status.code(),
          sc::loadLibrary(ctx, view(bytes), "", 0, 0).status.code(),
          sc::resolvePlaceholders(ctx).status.code(),
          sc::clearHistory(ctx).code()})
    {
      log += code == StatusCode::Busy ? "busy;" : "no;";
    }
  };
  fakehost::Obj cmd;
  cmd.methods["canRun"] = [&](void **args, void *ret, litestl::util::string *) {
    Context &ctx = *static_cast<Context *>(args[0]);
    log += sc::appendLibrary(ctx, view(bytes), "", 0, 0).status.code() == StatusCode::Busy
               ? "busy;"
               : "no;";
    new (ret) Status();
    return true;
  };
  cmd.methods["exec"] = [&](void **args, void *ret, litestl::util::string *) {
    Context &ctx = *static_cast<Context *>(args[0]);
    probe(ctx);
    log +=
        sc::appendLibrary(ctx, view(bytes), "", 0, 0).status.ok() ? "appended;" : "no;";
    new (ret) Status();
    return true;
  };
  cmd.methods["undo"] = [](void **, void *, litestl::util::string *) { return true; };
  cmd.methods["discard"] = [](void **, void *, litestl::util::string *err) {
    *err = "discard: Error: not today";
    return false;
  };
  cmd.methods["memSize"] = [](void **, void *ret, litestl::util::string *) {
    *static_cast<uint64_t *>(ret) = 0;
    return true;
  };
  fakehost::Obj factory;
  factory.methods["make"] = [&](void **, void *ret, litestl::util::string *) {
    *static_cast<uint32_t *>(ret) = host.add(cmd);
    return true;
  };
  CommandDef *def = sc::newCommandDef("js.append");
  test_assert(sc::registerForeignCommand(
                  c.ctx, *def, host.add(factory), "canRun exec undo memSize discard")
                  .ok());
  litestl::alloc::Delete(def);
  test_assert(sc::invoke(c.ctx, "js.append()").status.ok());
  if (log != "busy;busy;busy;busy;busy;appended;") {
    fprintf(stderr, "%s\n", log.c_str());
  }
  test_assert(log == "busy;busy;busy;busy;busy;appended;");
  test_assert(c.history.size() == 1 && c.w.lib.size() == 3);
  /* A discard that throws: cleared, and reported. */
  const Status cleared = sc::clearHistory(c.ctx);
  test_assert(strstr(cleared.message(), "not today") && !c.w.rt.hasStickyError() &&
              c.history.size() == 0);
}

int main()
{
  test_bytes();
  test_save();
  test_load();
  test_append();
  test_resolve();
  test_large();
  test_slots();
  return test_end();
}
