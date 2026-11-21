#pragma once

/** \file
 * Small text helpers shared by the parsers and the resolver.
 */

#include "coredcc/base/status.h"

#include <string>
#include <string_view>

namespace coredcc {

/** Edit distance counting insertions, deletions, substitutions and swaps of adjacent
 * characters as one each. Gives up (returning `limit + 1`) once it exceeds `limit`. */
int editDistance(std::string_view a, std::string_view b, int limit = 1 << 20);

/**
 * Collects "did you mean" candidates for a misspelled name: up to three of the closest
 * names within an edit distance of max(1, len / 3), closest first.
 */
class Suggester {
 public:
  explicit Suggester(std::string_view name);
  void consider(std::string_view candidate);
  /** Adds the candidates to `status` as suggestions. */
  void addTo(Status &status) const;
  int count() const
  {
    return count_;
  }

 private:
  std::string name_;
  int limit_;
  int count_ = 0;
  std::string best_[3];
  int dist_[3] = {};
};

/** True for [A-Za-z_][A-Za-z0-9_]*. */
bool isIdentifier(std::string_view s);

/**
 * Appends `s` double-quoted, escaping backslash, `"`, newline and tab: the escape set the
 * path and invocation parsers read back. `s` must not contain NUL.
 */
void appendQuoted(std::string &out, std::string_view s);

/** Appends the shortest decimal form of `v` that parses back to the same double. */
void appendDouble(std::string &out, double v);

} // namespace coredcc
