#include "coredcc/tool/event_script.h"

#include "coredcc/tool/manager.h"

#include <charconv>
#include <cmath>
#include <string>

namespace coredcc {

namespace {

struct Token {
  std::string_view text;
  size_t offset = 0;
};

Status bad(size_t offset, const std::string &why)
{
  return Status::error(
      StatusCode::InvalidArgument,
      ("event script at offset " + std::to_string(offset) + ": " + why).c_str());
}

bool number(std::string_view s, double &out)
{
  const char *end = s.data() + s.size();
  std::from_chars_result r = std::from_chars(s.data(), end, out);
  return r.ec == std::errc() && r.ptr == end && std::isfinite(out);
}

struct NamedKey {
  const char *name;
  int32_t key;
};

constexpr NamedKey kKeys[] = {
    {"escape", keys::kEscape},
    {"enter", keys::kEnter},
    {"tab", keys::kTab},
    {"backspace", keys::kBackspace},
    {"delete", keys::kDelete},
    {"left", keys::kLeft},
    {"right", keys::kRight},
    {"up", keys::kUp},
    {"down", keys::kDown},
    {"home", keys::kHome},
    {"end", keys::kEnd},
    {"pageup", keys::kPageUp},
    {"pagedown", keys::kPageDown},
    {"shift", keys::kShift},
    {"ctrl", keys::kCtrl},
    {"alt", keys::kAlt},
    {"meta", keys::kMeta},
};

bool keyOf(std::string_view name, int32_t &out)
{
  for (const NamedKey &k : kKeys) {
    if (name == k.name) {
      out = k.key;
      return true;
    }
  }
  if (name.size() >= 2 && name.size() <= 3 && name[0] == 'f') {
    double n = 0;
    if (number(name.substr(1), n) && n >= 1 && n <= 12 && n == std::floor(n)) {
      out = keys::kF1 + int32_t(n) - 1;
      return true;
    }
  }
  /* One printable ASCII character; others would need UTF-8 decoding. */
  if (name.size() == 1 && name[0] > ' ' && name[0] < 127) {
    out = int32_t(name[0]);
    return true;
  }
  return false;
}

/* The tokens of one statement. */
litestl::util::Vector<Token> split(std::string_view text, size_t begin, size_t end)
{
  litestl::util::Vector<Token> out;
  size_t i = begin;
  while (i < end) {
    while (i < end && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) {
      i++;
    }
    const size_t start = i;
    while (i < end && text[i] != ' ' && text[i] != '\t' && text[i] != '\r') {
      i++;
    }
    if (i > start) {
      out.append(Token{text.substr(start, i - start), start});
    }
  }
  return out;
}

class Parser {
public:
  litestl::util::Vector<InputEvent> events;

  Status statement(const litestl::util::Vector<Token> &t)
  {
    const std::string_view verb = t[0].text;
    int args = 0;
    if (verb == "down" || verb == "up" || verb == "move") {
      args = 2;
    } else if (verb == "drag") {
      args = 4;
    } else if (verb == "wheel") {
      args = 2;
    } else if (verb == "key" || verb == "keydown" || verb == "keyup") {
      args = 1;
    } else if (verb == "cancel" || verb == "focuslost") {
      args = 0;
    } else {
      return bad(t[0].offset, "unknown statement '" + std::string(verb) + "'");
    }
    if (int(t.size()) < 1 + args) {
      return bad(t[0].offset,
                 "'" + std::string(verb) + "' needs " + std::to_string(args) +
                     " arguments");
    }
    double num[4] = {0, 0, 0, 0};
    int32_t key = 0;
    if (args == 1) {
      if (!keyOf(t[1].text, key)) {
        return bad(t[1].offset, "unknown key '" + std::string(t[1].text) + "'");
      }
    } else {
      for (int i = 0; i < args; i++) {
        if (!number(t[1 + i].text, num[i])) {
          return bad(t[1 + i].offset,
                     "not a number: '" + std::string(t[1 + i].text) + "'");
        }
      }
    }
    /* Options. */
    InputEvent proto;
    proto.modifiers = 0;
    int32_t button = 0;
    int steps = 4;
    bool timed = false;
    for (int i = 1 + args; i < int(t.size()); i++) {
      const std::string_view o = t[i].text;
      const size_t eq = o.find('=');
      double v = 0;
      if (o == "shift" || o == "ctrl" || o == "alt" || o == "meta") {
        proto.modifiers |= o == "shift"  ? modifiers::kShift
                           : o == "ctrl" ? modifiers::kCtrl
                           : o == "alt"  ? modifiers::kAlt
                                         : modifiers::kMeta;
      } else if (o == "left" || o == "middle" || o == "right") {
        if (verb != "down" && verb != "up") {
          return bad(t[i].offset, "a button is only for down and up");
        }
        button = o == "left" ? 0 : o == "middle" ? 1 : 2;
      } else if (o == "pen" || o == "touch") {
        proto.pointer = o == "pen" ? PointerType::Pen : PointerType::Touch;
      } else if (eq != std::string_view::npos && number(o.substr(eq + 1), v)) {
        const std::string_view name = o.substr(0, eq);
        if (name == "pressure" && v >= 0 && v <= 1) {
          proto.pressure = v;
        } else if (name == "tiltx" && std::abs(v) <= 90) {
          proto.tiltX = v;
        } else if (name == "tilty" && std::abs(v) <= 90) {
          proto.tiltY = v;
        } else if (name == "time" && v >= 0) {
          proto.time = v;
          timed = true;
        } else if (name == "steps" && verb == "drag" && v >= 1 && v <= 10000 &&
                   v == std::floor(v))
        {
          steps = int(v);
        } else {
          return bad(t[i].offset, "bad option '" + std::string(o) + "'");
        }
      } else {
        return bad(t[i].offset, "unknown option '" + std::string(o) + "'");
      }
    }
    auto emit = [&](EventType type, double x, double y) {
      InputEvent e = proto;
      e.type = type;
      e.x = x;
      e.y = y;
      e.button = button;
      if (type == EventType::PointerDown) {
        buttons_ |= buttonBit(button);
      } else if (type == EventType::PointerUp) {
        buttons_ &= ~buttonBit(button);
      }
      e.buttons = buttons_;
      e.time = timed ? proto.time : (time_ += 1.0 / 60.0);
      time_ = e.time;
      if (e.isPointer()) {
        x_ = x;
        y_ = y;
      }
      events.append(e);
    };
    if (verb == "down") {
      emit(EventType::PointerDown, num[0], num[1]);
    } else if (verb == "up") {
      emit(EventType::PointerUp, num[0], num[1]);
    } else if (verb == "move") {
      emit(EventType::PointerMove, num[0], num[1]);
    } else if (verb == "drag") {
      emit(EventType::PointerDown, num[0], num[1]);
      timed = false; /* `time=` is the press's */
      for (int i = 1; i <= steps; i++) {
        const double f = double(i) / steps;
        emit(EventType::PointerMove,
             num[0] + (num[2] - num[0]) * f,
             num[1] + (num[3] - num[1]) * f);
      }
      emit(EventType::PointerUp, num[2], num[3]);
    } else if (verb == "wheel") {
      proto.wheelX = num[0];
      proto.wheelY = num[1];
      emit(EventType::Wheel, x_, y_);
    } else if (verb == "key" || verb == "keydown" || verb == "keyup") {
      proto.key = key;
      if (verb != "keyup") {
        emit(EventType::KeyDown, x_, y_);
      }
      if (verb != "keydown") {
        timed = false;
        emit(EventType::KeyUp, x_, y_);
      }
    } else {
      emit(verb == "cancel" ? EventType::Cancel : EventType::FocusLost, x_, y_);
    }
    return Status();
  }

private:
  static uint32_t buttonBit(int32_t button)
  {
    /* DOM: 1 left, 2 right, 4 middle. */
    return button == 0 ? 1u : button == 2 ? 2u : 4u;
  }
  uint32_t buttons_ = 0;
  double time_ = 0.0;
  double x_ = 0.0, y_ = 0.0;
};

} // namespace

Result<litestl::util::Vector<InputEvent>> parseEventScript(std::string_view text)
{
  Parser p;
  size_t i = 0;
  while (i <= text.size()) {
    size_t end = i;
    while (end < text.size() && text[end] != ';' && text[end] != '\n' && text[end] != '#')
    {
      end++;
    }
    litestl::util::Vector<Token> t = split(text, i, end);
    if (t.size() > 0) {
      CDCC_TRY(p.statement(t));
    }
    if (end < text.size() && text[end] == '#') {
      while (end < text.size() && text[end] != '\n') {
        end++;
      }
    }
    i = end + 1;
  }
  return std::move(p.events);
}

Status playEvents(ToolManager &tm,
                  Context &ctx,
                  const View &view,
                  std::span<const InputEvent> events,
                  litestl::util::Vector<int32_t> *handled)
{
  for (size_t i = 0; i < events.size(); i++) {
    Result<int32_t> r = tm.dispatch(ctx, view, events[i]);
    if (!r.ok()) {
      return Status::error(
          r.status().code(),
          ("event " + std::to_string(i) + ": " + r.status().message()).c_str());
    }
    if (handled) {
      handled->append(*r);
    }
  }
  return Status();
}

Status
playEventScript(ToolManager &tm, Context &ctx, const View &view, std::string_view text)
{
  Result<litestl::util::Vector<InputEvent>> events = parseEventScript(text);
  CDCC_TRY(events.status());
  return playEvents(
      tm, ctx, view, std::span<const InputEvent>(events->data(), events->size()));
}

} // namespace coredcc
