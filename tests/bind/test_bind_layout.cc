/* Layouts for scripts: their JSON, their issues, and tools under a prefix. */
#include "script_fixture.h"

#include "coredcc/bind/layout_facades.h"
#include "coredcc/layout/validate.h"

#include <chrono>

test_init;

namespace {

/* The radius of object i's mesh. */
double radius(Context &ctx, int i)
{
  return getValue(ctx, i ? "objects[1].data.radius" : "objects[0].data.radius")
      ->asFloat();
}

struct Grow {
  double before = 0;
  Status canRun(Context &) const
  {
    return Status();
  }
  Status exec(Context &ctx)
  {
    before = getValue(ctx, "radius").valueOr(PropValue::ofFloat(0)).asFloat();
    return setValue(ctx, "radius", PropValue::ofFloat(before + 1));
  }
  void undo(Context &ctx)
  {
    test_assert(setValue(ctx, "radius", PropValue::ofFloat(before)).ok());
  }
  uint64_t memSize() const
  {
    return 0;
  }
  void discard()
  {
  }
  static lbt::Struct<Grow> *defineBindings()
  {
    auto *st = new lbt::Struct<Grow>("test::Grow", sizeof(Grow));
    BIND_COMMAND_CORE(st);
    return st;
  }
};

bool is(const sc::StringResult &r, const char *want)
{
  if (!r.status.ok() || strcmp(r.value.c_str(), want) != 0) {
    fprintf(stderr, "got  %s%s\nwant %s\n", r.status.message(), r.value.c_str(), want);
  }
  return r.status.ok() && strcmp(r.value.c_str(), want) == 0;
}

} // namespace

static void test_json()
{
  test_assert(
      is(sc::parseLayout("<layout path='a'><prop path='b' id='x'/></layout>"),
         "{\"kind\":\"layout\",\"version\":1,\"path\":\"a\",\"effectivePath\":\"/"
         "a\",\"children\":[{"
         "\"kind\":\"prop\",\"path\":\"b\",\"effectivePath\":\"/a.b\",\"id\":\"x\"}]}"));
  sc::StringResult r = sc::parseLayout("<layout>\n  <prp/>\n</layout>");
  test_assert(r.status.code() == StatusCode::InvalidArgument && r.value.size() == 0 &&
              strstr(r.status.message(), "layout 2:3: ") &&
              r.status.suggestions().size() > 0);
}

static void test_issues()
{
  Setup s;
  test_assert(is(
      sc::validateLayout(s.ctx, "<layout><prop path='tool.strength'/></layout>"), "[]"));
  /* Lines and columns, in the issues' order. */
  test_assert(is(
      sc::validateLayout(s.ctx,
                         "<layout>\n <prop path='tool.strenth'/>\n"
                         "<tool label='\xC3\xA9' invocation='test.ad'/></layout>"),
      "[{\"severity\":\"Error\",\"line\":2,\"column\":8,\"node\":1,\"code\":\"NotFound\","
      "\"message\":\"<prop> path: no member 'strenth' in Tool (at offset 4)\","
      "\"suggestions\":[\"strength\"]},"
      "{\"severity\":\"Error\",\"line\":3,\"column\":17,\"node\":2,\"code\":\"NotFound\","
      "\"message\":\"<tool> invocation: no command 'test.ad'\","
      "\"suggestions\":[\"test.add\"]}]"));
  test_assert(!sc::validateLayout(s.ctx, "<layout>").status.ok());
  /* The root is the Context's, whatever its prefix. */
  s.ctx.setPrefix(*parsePath("tool"));
  test_assert(is(
      sc::validateLayout(s.ctx, "<layout><prop path='tool.strength'/></layout>"), "[]"));
  s.ctx.clearPrefix();
  Context none(&s.w.rt, &s.w.lib, PointerRef());
  sc::StringResult r = sc::validateLayout(none, "<layout/>");
  test_assert(r.status.ok() && strstr(r.value.c_str(), "isn't registered"));
}

static void test_issues_late()
{
  /* As many as there are, at the end of the largest text, in time. */
  Setup s;
  std::string xml =
      "<layout><custom tag='x'>" + std::string(900 * 1000, 'a') + "</custom>";
  for (int i = 0; i < 1500; i++) {
    xml += "<prop path='zzz'/>\n";
  }
  xml += "</layout>";
  const auto start = std::chrono::steady_clock::now();
  sc::StringResult r = sc::validateLayout(s.ctx, xml.c_str());
  const auto took = std::chrono::steady_clock::now() - start;
  test_assert(r.status.ok() && took < std::chrono::seconds(3));
  test_assert(strstr(r.value.c_str(), "\"line\":1000,\"column\":7,\"node\":1001,"));
}

static void test_invoke_at()
{
  Setup s;
  test_assert(addNativeCommand<Grow>(s.w.rt, CommandDef("test.grow")).ok());
  const double r1 = radius(s.ctx, 1), r0 = radius(s.ctx, 0);
  test_assert(sc::invokeAt(s.ctx, "objects[1].data", "test.grow()").status.ok());
  test_assert(radius(s.ctx, 1) == r1 + 1 && radius(s.ctx, 0) == r0 && !s.ctx.prefix());
  test_assert(sc::undo(s.ctx).ok() && radius(s.ctx, 1) == r1);
  test_assert(sc::redo(s.ctx).ok() && radius(s.ctx, 1) == r1 + 1);
  /* "" is the root, whatever the Context's prefix is. */
  s.ctx.setPrefix(*parsePath("objects[0].data"));
  test_assert(sc::invokeAt(s.ctx, "", "test.add(amount=1)").status.ok());
  test_assert(sc::invokeAt(s.ctx, "objects[1].data", "test.grow()").status.ok());
  test_assert(s.ctx.prefix() && *s.ctx.prefix() == *parsePath("objects[0].data"));
  s.ctx.clearPrefix();
  test_assert(radius(s.ctx, 1) == r1 + 2 && radius(s.ctx, 0) == r0);
  /* A prefix that isn't there now, isn't a struct, or isn't a path. */
  const int size = s.history.size();
  for (const char *prefix :
       {"objects[99]", "nothing", "tool.strength", "a..b", "objects"})
  {
    sc::BagResult r = sc::invokeAt(s.ctx, prefix, "test.add(amount=1)");
    test_assert(!r.status.ok() && !s.ctx.prefix());
  }
  test_assert(s.history.size() == size);
  test_assert(!sc::invokeAt(s.ctx, "tool", "test.nope()").status.ok());
}

static void test_slots()
{
  Setup s;
  Host host;
  Script script;
  test_assert(addNativeCommand<Grow>(s.w.rt, CommandDef("test.grow")).ok());
  test_assert(registerScript(s, host, &script, "js.tool", kCore).ok());
  const double r1 = radius(s.ctx, 1);
  Status inCanRun;
  script.onCanRun = [&](Context &ctx) {
    inCanRun = sc::invokeAt(ctx, "objects[1].data", "test.grow()").status;
  };
  script.onExec = [&](Context &ctx) {
    test_assert(sc::invokeAt(ctx, "objects[1].data", "test.grow()").status.ok());
  };
  test_assert(sc::invoke(s.ctx, "js.tool()").status.ok());
  test_assert(inCanRun.code() == StatusCode::Busy);
  /* A sub-command, undone with the command. */
  test_assert(s.history.size() == 1 && radius(s.ctx, 1) == r1 + 1);
  test_assert(sc::undo(s.ctx).ok() && radius(s.ctx, 1) == r1);
}

int main()
{
  test_json();
  test_issues();
  test_issues_late();
  test_invoke_at();
  test_slots();
  return test_end();
}
