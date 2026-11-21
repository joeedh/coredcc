#include "coredcc/parse/invocation.h"

#include "test_util.h"

#include <cstdio>
#include <cstring>
#include <string>

test_init;

using namespace coredcc;

static Invocation ok(const char *text)
{
  Result<Invocation> r = parseInvocation(text);
  if (!r.ok()) {
    fprintf(stderr, "parseInvocation('%s'): %s\n", text, r.status().message());
    test_assert(false);
    return Invocation();
  }
  return std::move(r).value();
}

static size_t failsAt(std::string_view text, const char *needle = nullptr)
{
  size_t off = 999;
  Result<Invocation> r = parseInvocation(text, &off);
  test_assert(!r.ok());
  if (r.ok()) {
    return 999;
  }
  if (needle && !strstr(r.status().message(), needle)) {
    fprintf(stderr, "message was: %s\n", r.status().message());
    test_assert(false);
  }
  return off;
}

static bool scalarIs(const InvocationArg *a, RawScalar::Kind kind, const char *text)
{
  return a && !a->value.isTuple && a->value.items.size() == 1 &&
         a->value.items[0].kind == kind && strcmp(a->value.items[0].text.c_str(), text) == 0;
}

static void test_forms()
{
  Invocation inv = ok("mesh.extrude(dist=1.0 mode='REGION')");
  test_assert(strcmp(inv.command.c_str(), "mesh.extrude") == 0);
  test_assert(inv.args.size() == 2);
  test_assert(scalarIs(inv.find("dist"), RawScalar::Kind::Number, "1.0"));
  test_assert(!inv.find("dist")->value.items[0].isInteger);
  test_assert(scalarIs(inv.find("mode"), RawScalar::Kind::String, "REGION"));
  test_assert(inv.find("mode")->offset == 22);

  inv = ok("view.frame");
  test_assert(inv.args.size() == 0 && inv.toString() == "view.frame");
  test_assert(ok("  view.frame ( )  ").args.size() == 0);

  inv = ok("object.transform(offset=(0, 0, 1.5), target=\"objects[\\\"Cube\\\"]\", n=-3)");
  const InvocationArg *offset = inv.find("offset");
  test_assert(offset && offset->value.isTuple && offset->value.items.size() == 3);
  test_assert(offset->value.items[2].kind == RawScalar::Kind::Number);
  test_assert(scalarIs(inv.find("target"), RawScalar::Kind::String, "objects[\"Cube\"]"));
  test_assert(inv.find("n")->value.items[0].isInteger);

  /* Separators: spaces, one comma, a trailing one; brackets for tuples. */
  test_assert(ok("c(a=1,b=2)") == ok("c(a=1 b=2)"));
  test_assert(ok("c(a=1 , b=2,)") == ok("c(a=1 b=2)"));
  test_assert(ok("c(v=[1 2,3,])") == ok("c(v=(1, 2, 3))"));
  test_assert(ok("c(v=())").find("v")->value.isTuple);
  test_assert(ok("c(v=(1))").find("v")->value.isTuple);
  test_assert(scalarIs(ok("c(flag=true)").find("flag"), RawScalar::Kind::Identifier, "true"));
  test_assert(scalarIs(ok("c(x=+5)").find("x"), RawScalar::Kind::Number, "5"));
  test_assert(scalarIs(ok("c(x=.5e-3)").find("x"), RawScalar::Kind::Number, ".5e-3"));
}

static void test_canonical()
{
  const char *inputs[] = {
      "mesh.extrude(dist=1.0 mode='REGION')",
      "c(v=[1 2,3,] s=\"a\\tb\" e=X)",
      "c",
      "c(x=-0 y=1e10 z=())",
  };
  for (const char *in : inputs) {
    const Invocation a = ok(in);
    const std::string canon = a.toString();
    test_assert(ok(canon.c_str()) == a);
    test_assert(ok(canon.c_str()).toString() == canon);
  }
  test_assert(ok("mesh.extrude(dist=1.0 mode='REGION')").toString() ==
              "mesh.extrude(dist=1.0 mode=\"REGION\")");
  test_assert(ok("c(v=[1 2,3,])").toString() == "c(v=(1, 2, 3))");
}

static void test_errors()
{
  test_assert(failsAt("", "empty invocation") == 0);
  test_assert(failsAt("   ", "empty invocation") == 3);
  test_assert(failsAt("mesh.") == 5);
  test_assert(failsAt("mesh.extrude(") == 13);
  test_assert(failsAt("c(a)", "expected '='") == 3);
  test_assert(failsAt("c(a=)", "expected a value") == 4);
  test_assert(failsAt("c(a=1 a=2)", "given twice") == 6);
  test_assert(failsAt("c(a=1,,b=2)") == 6);
  test_assert(failsAt("c(a=1b)", "after a number") == 5);
  test_assert(failsAt("c(a=1.2.3)") == 7);
  test_assert(failsAt("c(a=((1)))", "can't nest") == 5);
  test_assert(failsAt("c(a=(1 2)") == 9);
  test_assert(failsAt("c(a=1) x", "after the invocation") == 7);
  test_assert(failsAt("c(a=9223372036854775808)", "int64") == 4);
  test_assert(failsAt("c(a=-9223372036854775809)", "int64") == 4);
  test_assert(failsAt("c(a=1e999)", "out of range") == 4);
  test_assert(failsAt("c(a=1e)", "exponent") == 6);
  test_assert(failsAt("c(a=-)") == 4);
  test_assert(failsAt(std::string_view("c(a='x\0')", 9), "NUL") == 6);
  test_assert(failsAt("c(a='\\x')", "unknown escape") == 5);
  test_assert(failsAt("c(a='x)", "unterminated") == 7);
  test_assert(failsAt("c[a=1]") == 1);
}

int main()
{
  test_forms();
  test_canonical();
  test_errors();
  return test_end();
}
