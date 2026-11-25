#include "coredcc/api/ref_text.h"

#include "coredcc/api/struct.h"
#include "coredcc/base/text.h"
#include "coredcc/lib/registry.h"
#include "coredcc/parse/lex.h"
#include "coredcc/parse/path.h"

#include <cinttypes>
#include <cstdio>

namespace coredcc {

using parse_detail::Failure;
using parse_detail::Lexer;

namespace {

bool isDottedIdentifier(std::string_view s)
{
  while (true) {
    const size_t dot = s.find('.');
    if (!isIdentifier(s.substr(0, dot))) {
      return false;
    }
    if (dot == std::string_view::npos) {
      return true;
    }
    s.remove_prefix(dot + 1);
  }
}

void appendHex(std::string &out, uint64_t v)
{
  char buf[20];
  snprintf(buf, sizeof(buf), "%016" PRIx64, v);
  out += buf;
}

/* `name` may be null: the id itself then. */
void appendName(std::string &out, const char *name, uint64_t id)
{
  if (!name) {
    out += '#';
    appendHex(out, id);
  }
  else if (isDottedIdentifier(name)) {
    out += name;
  }
  else {
    appendQuoted(out, name);
  }
}

int hexDigit(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

class RefParser {
 public:
  explicit RefParser(std::string_view text) : lx_(text) {}

  bool parse(StableRef &out)
  {
    lx_.skipSpace();
    if (lx_.atEnd()) {
      return true;
    }
    if (!token('@') || !owner(out.owner)) {
      return false;
    }
    for (lx_.skipSpace(); lx_.peek() == '.' || lx_.peek() == '['; lx_.skipSpace()) {
      if (!(lx_.eat('.') ? member(out) : subscript(out))) {
        return false;
      }
    }
    if (lx_.eat(':') && !name(IdDomain::Struct, "", out.type.value)) {
      return false;
    }
    lx_.skipSpace();
    return lx_.atEnd() || expected("the end");
  }

  Failure fail;

 private:
  bool expected(const std::string &what)
  {
    fail = lx_.failHere("expected " + what + ", got " + lx_.describeHere());
    return false;
  }
  bool token(char c)
  {
    lx_.skipSpace();
    return lx_.eat(c) || expected(std::string("'") + c + "'");
  }

  /* 16 digits. */
  bool hex(uint64_t &out)
  {
    out = 0;
    for (int i = 0; i < 16; i++) {
      const int d = hexDigit(lx_.peek());
      if (d < 0) {
        return expected("a hexadecimal digit");
      }
      lx_.eat(lx_.peek());
      out = out << 4 | uint64_t(d);
    }
    return true;
  }

  bool isQuote() const
  {
    return lx_.peek() == '"' || lx_.peek() == '\'';
  }

  bool name(IdDomain domain, const std::string &prefix, uint64_t &id)
  {
    lx_.skipSpace();
    const size_t start = lx_.pos();
    std::string text;
    if (lx_.eat('#')) {
      if (!hex(id)) {
        return false;
      }
    }
    else if (isQuote()) {
      if (!lx_.quoted(text, fail)) {
        return false;
      }
      id = text.empty() ? 0 : hashId(domain, prefix + text);
    }
    else {
      std::string_view ident;
      do {
        if (!lx_.identifier(ident)) {
          return expected("a name");
        }
        text += text.empty() ? "" : ".";
        text += ident;
      } while (lx_.eat('.'));
      id = hashId(domain, prefix + text);
    }
    if (id == 0) {
      fail = Failure{start, "an empty name"};
      return false;
    }
    return true;
  }

  bool owner(OwnerKey &out)
  {
    lx_.skipSpace();
    const size_t start = lx_.pos();
    std::string_view kind;
    if (!lx_.identifier(kind) || (kind != "block" && kind != "owner")) {
      fail = Failure{start, "expected 'block' or 'owner' after '@'"};
      return false;
    }
    if (!token('(')) {
      return false;
    }
    if (kind == "owner") {
      out.kind = OwnerKey::Kind::Runtime;
      return name(IdDomain::Type, kOwnerNamePrefix, out.nameHash) && token(')');
    }
    out.kind = OwnerKey::Kind::Block;
    if (!name(IdDomain::Client, "", out.block.client.value) || !token(',')) {
      return false;
    }
    lx_.skipSpace();
    const size_t at = lx_.pos();
    if (!hex(out.block.local.hi) || !hex(out.block.local.lo)) {
      return false;
    }
    if (out.isNull()) {
      fail = Failure{at, "a null id (the empty text is the null pointer)"};
      return false;
    }
    return token(')');
  }

  bool member(StableRef &out)
  {
    lx_.skipSpace();
    std::string text;
    std::string_view ident;
    if (isQuote()) {
      if (!lx_.quoted(text, fail)) {
        return false;
      }
    }
    else if (lx_.identifier(ident)) {
      text = ident;
    }
    else {
      return expected("a member name");
    }
    out.key.append(KeySegment::member(text));
    return true;
  }

  bool subscript(StableRef &out)
  {
    lx_.eat('[');
    lx_.skipSpace();
    if (lx_.eat('#')) {
      int64_t handle = 0;
      if (!lx_.int64(handle, fail)) {
        return false;
      }
      out.key.append(KeySegment::ofHandle(handle));
    }
    else {
      std::string text;
      if (!isQuote()) {
        return expected("a quoted key or '#handle'");
      }
      if (!lx_.quoted(text, fail)) {
        return false;
      }
      out.key.append(KeySegment::stringKey(text));
    }
    return token(']');
  }

  Lexer lx_;
};

} // namespace

std::string formatRef(const Runtime &rt, const StableRef &ref)
{
  std::string out;
  if (ref.isNull()) {
    return out;
  }
  if (ref.owner.kind == OwnerKey::Kind::Block) {
    const BlockId &id = ref.owner.block;
    out += "@block(";
    appendName(out, rt.nameOf(id.client), id.client.value);
    out += ", ";
    appendHex(out, id.local.hi);
    appendHex(out, id.local.lo);
  }
  else {
    const char *name = rt.nameOf(IdDomain::Type, ref.owner.nameHash);
    const std::string_view prefix = kOwnerNamePrefix;
    const bool isOwner = name && std::string_view(name).substr(0, prefix.size()) == prefix;
    out += "@owner(";
    appendName(out, isOwner ? name + prefix.size() : nullptr, ref.owner.nameHash);
  }
  out += ')';
  for (const KeySegment &s : ref.key) {
    switch (s.kind) {
      case KeySegment::Kind::Member:
        out += '.';
        if (isIdentifier(s.text.c_str())) {
          out += s.text.c_str();
        }
        else {
          appendQuoted(out, s.text.c_str());
        }
        break;
      case KeySegment::Kind::StringKey:
        out += '[';
        appendQuoted(out, s.text.c_str());
        out += ']';
        break;
      case KeySegment::Kind::Handle:
        out += "[#" + std::to_string(s.handle) + ']';
        break;
    }
  }
  if (ref.type) {
    out += " : ";
    appendName(out, rt.nameOf(ref.type), ref.type.value);
  }
  return out;
}

Result<StableRef> parseRef(const Runtime &rt, std::string_view text, size_t *errorOffset)
{
  RefParser p(text);
  StableRef ref;
  bool ok = false;
  if (text.size() > kMaxPathLength) {
    p.fail = Failure{0, "longer than " + std::to_string(kMaxPathLength) + " bytes"};
  }
  else {
    ok = p.parse(ref);
  }
  if (!ok) {
    if (errorOffset) {
      *errorOffset = p.fail.offset;
    }
    return parse_detail::toStatus("reference", p.fail);
  }
  if (const ClientRegistry *clients = rt.findExt<ClientRegistry>()) {
    ref.owner.block = clients->canonical(ref.owner.block);
  }
  return ref;
}

} // namespace coredcc
