#pragma once

/** \file
 * Lexing shared by the path and invocation parsers. Internal to coredcc_parse.
 */

#include "coredcc/base/status.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace coredcc::parse_detail {

/** A parse failure: where, and why. */
struct Failure {
  size_t offset = 0;
  std::string what;
};

class Lexer {
 public:
  explicit Lexer(std::string_view text) : s_(text) {}

  size_t pos() const
  {
    return pos_;
  }
  bool atEnd() const
  {
    return pos_ >= s_.size();
  }
  char peek(size_t ahead = 0) const
  {
    return pos_ + ahead < s_.size() ? s_[pos_ + ahead] : '\0';
  }
  bool eat(char c)
  {
    if (!atEnd() && s_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }
  void skipSpace();

  /** [A-Za-z_][A-Za-z0-9_]*, or false (consuming nothing). */
  bool identifier(std::string_view &out);
  /** A non-negative decimal integer that fits int64. */
  bool int64(int64_t &out, Failure &fail);
  /**
   * A '...' or "..." string with escapes \\ \" \' \n \t. Rejects other escapes, NUL
   * (raw or escaped) and a missing closing quote.
   */
  bool quoted(std::string &out, Failure &fail);
  /**
   * A number: [+-]? (digits [. digits*] | . digits) ([eE] [+-]? digits)?. `isInteger`
   * is set when it has no fraction or exponent; integers must fit int64, other numbers
   * must be finite doubles. The leading '+' is dropped from `text`.
   */
  bool number(std::string &text, bool &isInteger, Failure &fail);

  Failure failHere(std::string what) const
  {
    return Failure{pos_, std::move(what)};
  }
  /** "unexpected 'x'", "unexpected NUL" or "unexpected end of input". */
  std::string describeHere() const;

 private:
  std::string_view s_;
  size_t pos_ = 0;
};

bool isIdentStart(char c);
bool isIdentChar(char c);
bool isDigit(char c);

/** The error Status for `fail` while parsing a `kind` ("path", "invocation"). */
Status toStatus(const char *kind, const Failure &fail);

} // namespace coredcc::parse_detail
