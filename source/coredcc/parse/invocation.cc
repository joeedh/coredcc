#include "coredcc/parse/invocation.h"

#include "coredcc/base/text.h"
#include "coredcc/parse/lex.h"

#include <cstring>

namespace coredcc {

using litestl::util::string;
using parse_detail::Failure;
using parse_detail::Lexer;

bool RawScalar::operator==(const RawScalar &b) const
{
  return kind == b.kind && isInteger == b.isInteger && strcmp(text.c_str(), b.text.c_str()) == 0;
}

bool RawValue::operator==(const RawValue &b) const
{
  if (isTuple != b.isTuple || items.size() != b.items.size()) {
    return false;
  }
  for (int i = 0; i < int(items.size()); i++) {
    if (!(items[i] == b.items[i])) {
      return false;
    }
  }
  return true;
}

const InvocationArg *Invocation::find(std::string_view name) const
{
  for (const InvocationArg &a : args) {
    if (name == a.name.c_str()) {
      return &a;
    }
  }
  return nullptr;
}

bool Invocation::operator==(const Invocation &b) const
{
  if (strcmp(command.c_str(), b.command.c_str()) != 0 || args.size() != b.args.size()) {
    return false;
  }
  for (int i = 0; i < int(args.size()); i++) {
    if (strcmp(args[i].name.c_str(), b.args[i].name.c_str()) != 0 ||
        !(args[i].value == b.args[i].value))
    {
      return false;
    }
  }
  return true;
}

namespace {

void appendScalar(std::string &out, const RawScalar &s)
{
  if (s.kind == RawScalar::Kind::String) {
    appendQuoted(out, s.text.c_str());
  }
  else {
    out += s.text.c_str();
  }
}

} // namespace

std::string Invocation::toString() const
{
  std::string out = command.c_str();
  if (args.size() == 0) {
    return out;
  }
  out += '(';
  for (int i = 0; i < int(args.size()); i++) {
    if (i) {
      out += ' ';
    }
    out += args[i].name.c_str();
    out += '=';
    const RawValue &v = args[i].value;
    if (v.isTuple) {
      out += '(';
    }
    for (int j = 0; j < int(v.items.size()); j++) {
      if (j) {
        out += ", ";
      }
      appendScalar(out, v.items[j]);
    }
    if (v.isTuple) {
      out += ')';
    }
  }
  out += ')';
  return out;
}

namespace {

string toLs(std::string_view s)
{
  return string(std::string(s).c_str());
}

bool parseScalar(Lexer &lx, RawScalar &out, Failure &fail)
{
  const char c = lx.peek();
  std::string_view ident;
  if (c == '"' || c == '\'') {
    std::string s;
    if (!lx.quoted(s, fail)) {
      return false;
    }
    out.kind = RawScalar::Kind::String;
    out.text = toLs(s);
    return true;
  }
  if (lx.identifier(ident)) {
    out.kind = RawScalar::Kind::Identifier;
    out.text = toLs(ident);
    return true;
  }
  if (parse_detail::isDigit(c) || c == '-' || c == '+' || c == '.') {
    std::string num;
    if (!lx.number(num, out.isInteger, fail)) {
      return false;
    }
    out.kind = RawScalar::Kind::Number;
    out.text = toLs(num);
    return true;
  }
  fail = lx.failHere("expected a value, got " + lx.describeHere());
  return false;
}

/* Skips a separator: whitespace, or one comma with whitespace around it. Returns
 * whether there was one. */
bool separator(Lexer &lx)
{
  const size_t start = lx.pos();
  lx.skipSpace();
  if (lx.eat(',')) {
    lx.skipSpace();
  }
  return lx.pos() != start;
}

bool parseValue(Lexer &lx, RawValue &out, Failure &fail)
{
  const char open = lx.peek();
  if (open != '(' && open != '[') {
    RawScalar s;
    if (!parseScalar(lx, s, fail)) {
      return false;
    }
    out.items.append(std::move(s));
    return true;
  }
  const char close = open == '(' ? ')' : ']';
  lx.eat(open);
  out.isTuple = true;
  lx.skipSpace();
  if (lx.eat(close)) {
    return true;
  }
  while (true) {
    if (lx.peek() == '(' || lx.peek() == '[') {
      fail = lx.failHere("tuples can't nest");
      return false;
    }
    RawScalar s;
    if (!parseScalar(lx, s, fail)) {
      return false;
    }
    out.items.append(std::move(s));
    const bool sep = separator(lx);
    if (lx.eat(close)) {
      return true; /* a trailing separator is fine */
    }
    if (!sep) {
      fail = lx.failHere(std::string("expected ',' or '") + close + "', got " +
                         lx.describeHere());
      return false;
    }
  }
}

bool parseArgs(Lexer &lx, Invocation &out, Failure &fail)
{
  lx.skipSpace();
  if (lx.eat(')')) {
    return true;
  }
  while (true) {
    InvocationArg arg;
    arg.offset = lx.pos();
    std::string_view name;
    if (!lx.identifier(name)) {
      fail = lx.failHere("expected an argument name, got " + lx.describeHere());
      return false;
    }
    if (out.find(name)) {
      fail = Failure{arg.offset, "argument '" + std::string(name) + "' given twice"};
      return false;
    }
    arg.name = toLs(name);
    lx.skipSpace();
    if (!lx.eat('=')) {
      fail = lx.failHere("expected '=' after '" + std::string(name) + "', got " +
                         lx.describeHere());
      return false;
    }
    lx.skipSpace();
    if (!parseValue(lx, arg.value, fail)) {
      return false;
    }
    out.args.append(std::move(arg));
    const bool sep = separator(lx);
    if (lx.eat(')')) {
      return true; /* a trailing separator is fine */
    }
    if (!sep) {
      fail = lx.failHere("expected ',' or ')', got " + lx.describeHere());
      return false;
    }
  }
}

bool parseInto(Lexer &lx, Invocation &out, Failure &fail)
{
  lx.skipSpace();
  std::string_view part;
  if (!lx.identifier(part)) {
    fail = lx.failHere(lx.atEnd() ? std::string("empty invocation") :
                                    "expected a command id, got " + lx.describeHere());
    return false;
  }
  std::string id(part);
  while (lx.eat('.')) {
    if (!lx.identifier(part)) {
      fail = lx.failHere("expected a name after '.', got " + lx.describeHere());
      return false;
    }
    id += '.';
    id += part;
  }
  out.command = toLs(id);
  lx.skipSpace();
  if (lx.eat('(') && !parseArgs(lx, out, fail)) {
    return false;
  }
  lx.skipSpace();
  if (!lx.atEnd()) {
    fail = lx.failHere("unexpected " + lx.describeHere() + " after the invocation");
    return false;
  }
  return true;
}

} // namespace

Result<Invocation> parseInvocation(std::string_view text, size_t *errorOffset)
{
  Lexer lx(text);
  Invocation inv;
  Failure fail;
  if (!parseInto(lx, inv, fail)) {
    if (errorOffset) {
      *errorOffset = fail.offset;
    }
    return parse_detail::toStatus("invocation", fail);
  }
  return inv;
}

} // namespace coredcc
