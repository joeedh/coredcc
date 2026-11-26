/* The text form of input events (tool/event_script.h). */
#include "coredcc/tool/event_script.h"

#include "test_util.h"

#include <cmath>
#include <string>

test_init;

using namespace coredcc;
using litestl::util::Vector;

static Vector<InputEvent> parse(const char *text)
{
  Result<Vector<InputEvent>> r = parseEventScript(text);
  test_assert(r.ok());
  return r.ok() ? std::move(r).value() : Vector<InputEvent>();
}

/* The offset that the error names, or -1. */
static long errorOffset(const char *text)
{
  Result<Vector<InputEvent>> r = parseEventScript(text);
  test_assert(!r.ok() && r.status().code() == StatusCode::InvalidArgument);
  const std::string m = r.status().message();
  const size_t at = m.find("offset ");
  return at == std::string::npos ? -1 : std::stol(m.substr(at + 7));
}

static void test_statements()
{
  Vector<InputEvent> e =
      parse("down 400 300; move 500 300 shift; up 500 300;\nkey escape");
  test_assert(e.size() == 5);
  test_assert(e[0].type == EventType::PointerDown && e[0].x == 400 && e[0].y == 300);
  test_assert(e[0].button == 0 && e[0].buttons == 1);
  test_assert(e[1].type == EventType::PointerMove && e[1].has(modifiers::kShift));
  test_assert(e[1].buttons == 1 && !e[0].has(modifiers::kShift));
  test_assert(e[2].type == EventType::PointerUp && e[2].buttons == 0);
  test_assert(e[3].type == EventType::KeyDown && e[3].key == keys::kEscape);
  test_assert(e[4].type == EventType::KeyUp && e[4].key == keys::kEscape);
  /* Times are 1/60 s apart. */
  test_assert(std::abs(e[4].time - 5.0 / 60.0) < 1e-12 && e[0].time > 0);

  e = parse("drag 0 0 100 50 steps=5 ctrl");
  test_assert(e.size() == 7 && e[0].type == EventType::PointerDown);
  test_assert(e[5].type == EventType::PointerMove && e[5].x == 100 && e[5].y == 50);
  test_assert(e[1].x == 20 && e[1].y == 10 && e[6].type == EventType::PointerUp);
  test_assert(e[3].has(modifiers::kCtrl));

  e = parse(
      "down 1 2 right pen pressure=0.25 tiltx=-30 time=2; wheel 0 -120; key x; key f12; "
      "keydown /; cancel # a comment ; down 9 9\nfocuslost");
  test_assert(e.size() == 9);
  test_assert(e[0].button == 2 && e[0].buttons == 2 && e[0].pointer == PointerType::Pen);
  test_assert(e[0].pressure == 0.25 && e[0].tiltX == -30 && e[0].time == 2.0);
  test_assert(e[1].type == EventType::Wheel && e[1].wheelY == -120);
  test_assert(e[1].x == 1 && e[1].y == 2 && e[1].time > 2.0);
  test_assert(e[2].key == 'x' && e[4].key == keys::kF1 + 11 && e[6].key == '/');
  test_assert(e[7].type == EventType::Cancel && e[8].type == EventType::FocusLost);
  test_assert(parse("").size() == 0 && parse(" ;;\n # only\n").size() == 0);
}

static void test_errors()
{
  test_assert(errorOffset("down 1 2; jump 3 4") == 10);
  test_assert(errorOffset("down 1") == 0);
  test_assert(errorOffset("move 1 x") == 7);
  test_assert(errorOffset("move 1 2 sideways") == 9);
  test_assert(errorOffset("key nokey") == 4);
  test_assert(errorOffset("move 1 2 right") == 9);
  test_assert(errorOffset("down 1 2 pressure=2") == 9);
  test_assert(errorOffset("drag 0 0 1 1 steps=0") == 13);
  test_assert(errorOffset("move nan 2") == 5);
  test_assert(errorOffset("key f13") == 4);
}

int main()
{
  test_statements();
  test_errors();
  return test_end();
}
