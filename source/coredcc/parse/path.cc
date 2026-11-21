#include "coredcc/parse/path.h"

#include "coredcc/base/id.h"
#include "coredcc/base/text.h"
#include "coredcc/parse/lex.h"

#include <cstring>

namespace coredcc {

using litestl::util::string;
using parse_detail::Failure;
using parse_detail::Lexer;

bool PathSegment::operator==(const PathSegment &b) const
{
  if (kind != b.kind) {
    return false;
  }
  if (kind == SegmentKind::Index || kind == SegmentKind::HandleKey) {
    return index == b.index;
  }
  return strcmp(text.c_str(), b.text.c_str()) == 0;
}

bool Path::operator==(const Path &b) const
{
  if (segments.size() != b.segments.size()) {
    return false;
  }
  for (int i = 0; i < size(); i++) {
    if (!(segments[i] == b.segments[i])) {
      return false;
    }
  }
  return true;
}

uint64_t Path::computeHash() const
{
  uint64_t h = detail::kFnvOffset;
  for (const PathSegment &s : segments) {
    const char k = char(s.kind);
    h = detail::fnv1a64(std::string_view(&k, 1), h);
    if (s.kind == SegmentKind::Index || s.kind == SegmentKind::HandleKey) {
      h = detail::fnv1a64(
          std::string_view(reinterpret_cast<const char *>(&s.index), sizeof(s.index)), h);
    }
    else {
      h = detail::fnv1a64(s.text.c_str(), h);
    }
  }
  return h;
}

std::string Path::toString() const
{
  std::string out;
  for (int i = 0; i < size(); i++) {
    const PathSegment &s = segments[i];
    switch (s.kind) {
      case SegmentKind::Member:
        if (i > 0) {
          out += '.';
        }
        out += s.text.c_str();
        break;
      case SegmentKind::Index:
        out += '[' + std::to_string(s.index) + ']';
        break;
      case SegmentKind::StringKey:
        out += '[';
        appendQuoted(out, s.text.c_str());
        out += ']';
        break;
      case SegmentKind::HandleKey:
        out += "[#" + std::to_string(s.index) + ']';
        break;
      case SegmentKind::EnumKey:
        out += '[';
        out += s.text.c_str();
        out += ']';
        break;
    }
  }
  return out;
}

namespace {

PathSegment named(SegmentKind kind, std::string_view text)
{
  PathSegment s;
  s.kind = kind;
  s.text = string(std::string(text).c_str());
  return s;
}

PathSegment indexed(SegmentKind kind, int64_t index)
{
  PathSegment s;
  s.kind = kind;
  s.index = index;
  return s;
}

bool parseSubscript(Lexer &lx, Path &out, Failure &fail)
{
  const char c = lx.peek();
  std::string_view ident;
  if (parse_detail::isDigit(c)) {
    int64_t v = 0;
    if (!lx.int64(v, fail)) {
      return false;
    }
    out.segments.append(indexed(SegmentKind::Index, v));
  }
  else if (c == '#') {
    lx.eat('#');
    int64_t v = 0;
    if (!lx.int64(v, fail)) {
      return false;
    }
    out.segments.append(indexed(SegmentKind::HandleKey, v));
  }
  else if (c == '"' || c == '\'') {
    std::string key;
    if (!lx.quoted(key, fail)) {
      return false;
    }
    out.segments.append(named(SegmentKind::StringKey, key));
  }
  else if (lx.identifier(ident)) {
    out.segments.append(named(SegmentKind::EnumKey, ident));
  }
  else {
    fail = lx.failHere("expected an index, a quoted key, '#handle' or a key, got " +
                       lx.describeHere());
    return false;
  }
  if (!lx.eat(']')) {
    fail = lx.failHere("expected ']', got " + lx.describeHere());
    return false;
  }
  return true;
}

bool parseInto(Lexer &lx, Path &out, Failure &fail)
{
  std::string_view name;
  if (lx.atEnd()) {
    fail = lx.failHere("empty path");
    return false;
  }
  if (!lx.identifier(name)) {
    fail = lx.failHere("expected a member name, got " + lx.describeHere());
    return false;
  }
  out.segments.append(named(SegmentKind::Member, name));
  while (!lx.atEnd()) {
    if (lx.eat('.')) {
      if (!lx.identifier(name)) {
        fail = lx.failHere("expected a member name after '.', got " + lx.describeHere());
        return false;
      }
      out.segments.append(named(SegmentKind::Member, name));
    }
    else if (lx.eat('[')) {
      if (!parseSubscript(lx, out, fail)) {
        return false;
      }
    }
    else {
      fail = lx.failHere("expected '.' or '[', got " + lx.describeHere());
      return false;
    }
  }
  return true;
}

} // namespace

Result<Path> parsePath(std::string_view text, size_t *errorOffset)
{
  Lexer lx(text);
  Path path;
  Failure fail;
  if (!parseInto(lx, path, fail)) {
    if (errorOffset) {
      *errorOffset = fail.offset;
    }
    return parse_detail::toStatus("path", fail);
  }
  return path;
}

} // namespace coredcc
