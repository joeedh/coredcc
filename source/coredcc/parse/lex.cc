#include "coredcc/parse/lex.h"

#include <charconv>
#include <cmath>
#include <cstdio>

namespace coredcc::parse_detail {

bool isIdentStart(char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c)
{
  return isIdentStart(c) || isDigit(c);
}

bool isDigit(char c)
{
  return c >= '0' && c <= '9';
}

void Lexer::skipSpace()
{
  while (!atEnd() && (s_[pos_] == ' ' || s_[pos_] == '\t')) {
    pos_++;
  }
}

bool Lexer::identifier(std::string_view &out)
{
  if (atEnd() || !isIdentStart(s_[pos_])) {
    return false;
  }
  const size_t start = pos_;
  while (!atEnd() && isIdentChar(s_[pos_])) {
    pos_++;
  }
  out = s_.substr(start, pos_ - start);
  return true;
}

bool Lexer::int64(int64_t &out, Failure &fail)
{
  if (atEnd() || !isDigit(s_[pos_])) {
    fail = failHere("expected an integer, got " + describeHere());
    return false;
  }
  const size_t start = pos_;
  uint64_t v = 0;
  bool overflow = false;
  while (!atEnd() && isDigit(s_[pos_])) {
    const uint64_t d = uint64_t(s_[pos_] - '0');
    if (v > (uint64_t(INT64_MAX) - d) / 10) {
      overflow = true;
    }
    if (!overflow) {
      v = v * 10 + d;
    }
    pos_++;
  }
  if (overflow) {
    fail = Failure{start, "integer out of the int64 range"};
    return false;
  }
  out = int64_t(v);
  return true;
}

bool Lexer::quoted(std::string &out, Failure &fail)
{
  const char quote = peek();
  if (quote != '"' && quote != '\'') {
    fail = failHere("expected a quoted string, got " + describeHere());
    return false;
  }
  const size_t start = pos_;
  pos_++;
  out.clear();
  while (true) {
    if (atEnd()) {
      fail =
          Failure{s_.size(),
                  "unterminated string (opened at offset " + std::to_string(start) + ")"};
      return false;
    }
    const char c = s_[pos_];
    if (c == '\0') {
      fail = failHere("NUL in a string");
      return false;
    }
    if (c == quote) {
      pos_++;
      return true;
    }
    if (c != '\\') {
      out += c;
      pos_++;
      continue;
    }
    const char e = peek(1);
    switch (e) {
    case '\\':
    case '"':
    case '\'':
      out += e;
      break;
    case 'n':
      out += '\n';
      break;
    case 't':
      out += '\t';
      break;
    default:
      if (pos_ + 1 >= s_.size()) {
        fail = Failure{s_.size(), "unterminated escape"};
      } else {
        fail = failHere("unknown escape (allowed: \\\\ \\\" \\' \\n \\t)");
      }
      return false;
    }
    pos_ += 2;
  }
}

bool Lexer::number(std::string &text, bool &isInteger, Failure &fail)
{
  const size_t start = pos_;
  size_t i = pos_;
  if (i < s_.size() && (s_[i] == '+' || s_[i] == '-')) {
    i++;
  }
  size_t intDigits = 0;
  while (i < s_.size() && isDigit(s_[i])) {
    i++;
    intDigits++;
  }
  size_t fracDigits = 0;
  bool dot = false;
  if (i < s_.size() && s_[i] == '.') {
    dot = true;
    i++;
    while (i < s_.size() && isDigit(s_[i])) {
      i++;
      fracDigits++;
    }
  }
  if (intDigits + fracDigits == 0) {
    fail = failHere("expected a number, got " + describeHere());
    return false;
  }
  bool exponent = false;
  if (i < s_.size() && (s_[i] == 'e' || s_[i] == 'E')) {
    size_t j = i + 1;
    if (j < s_.size() && (s_[j] == '+' || s_[j] == '-')) {
      j++;
    }
    if (j >= s_.size() || !isDigit(s_[j])) {
      fail = Failure{j, "expected exponent digits"};
      return false;
    }
    while (j < s_.size() && isDigit(s_[j])) {
      j++;
    }
    exponent = true;
    i = j;
  }
  if (i < s_.size() && isIdentChar(s_[i])) {
    fail = Failure{i, "unexpected '" + std::string(1, s_[i]) + "' after a number"};
    return false;
  }
  std::string_view num = s_.substr(start, i - start);
  if (num[0] == '+') {
    num.remove_prefix(1);
  }
  isInteger = !dot && !exponent;
  if (isInteger) {
    int64_t v = 0;
    std::from_chars_result r = std::from_chars(num.data(), num.data() + num.size(), v);
    if (r.ec != std::errc() || r.ptr != num.data() + num.size()) {
      fail = Failure{start, "integer out of the int64 range"};
      return false;
    }
  } else {
    double v = 0.0;
    std::from_chars_result r = std::from_chars(num.data(), num.data() + num.size(), v);
    if (r.ec != std::errc() || r.ptr != num.data() + num.size() || !std::isfinite(v)) {
      fail = Failure{start, "number out of range"};
      return false;
    }
  }
  text.assign(num);
  pos_ = i;
  return true;
}

std::string Lexer::describeHere() const
{
  if (atEnd()) {
    return "end of input";
  }
  const char c = s_[pos_];
  if (c == '\0') {
    return "NUL";
  }
  if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
    char buf[16];
    snprintf(buf, sizeof(buf), "byte 0x%02x", static_cast<unsigned char>(c));
    return buf;
  }
  return "'" + std::string(1, c) + "'";
}

Status toStatus(const char *kind, const Failure &fail)
{
  return Status::error(
      StatusCode::InvalidArgument,
      (std::string(kind) + ": " + fail.what + " at offset " + std::to_string(fail.offset))
          .c_str());
}

} // namespace coredcc::parse_detail
