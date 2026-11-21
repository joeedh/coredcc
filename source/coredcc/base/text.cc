#include "coredcc/base/text.h"

#include "litestl/util/vector.h"

#include <algorithm>
#include <charconv>

namespace coredcc {

int editDistance(std::string_view a, std::string_view b, int limit)
{
  const int n = int(a.size());
  const int m = int(b.size());
  if ((std::max)(n, m) - (std::min)(n, m) > limit) {
    return limit + 1;
  }
  /* Optimal string alignment: Levenshtein plus adjacent transpositions, with three rows
   * (two back, one back, current). */
  litestl::util::Vector<int> prev2, prev, cur;
  prev2.resize(m + 1);
  prev.resize(m + 1);
  cur.resize(m + 1);
  for (int j = 0; j <= m; j++) {
    prev[j] = j;
  }
  for (int i = 1; i <= n; i++) {
    cur[0] = i;
    int rowMin = cur[0];
    for (int j = 1; j <= m; j++) {
      const int cost = a[i - 1] == b[j - 1] ? 0 : 1;
      int d = (std::min)({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1]) {
        d = (std::min)(d, prev2[j - 2] + 1);
      }
      cur[j] = d;
      rowMin = (std::min)(rowMin, d);
    }
    if (rowMin > limit) {
      return limit + 1;
    }
    std::swap(prev2, prev);
    std::swap(prev, cur);
  }
  return (std::min)(prev[m], limit + 1);
}

Suggester::Suggester(std::string_view name)
    : name_(name), limit_((std::max)(1, int(name.size()) / 3))
{
}

void Suggester::consider(std::string_view candidate)
{
  const int d = editDistance(name_, candidate, limit_);
  if (d > limit_ || candidate == name_) {
    return;
  }
  for (int i = 0; i < count_; i++) {
    if (best_[i] == candidate) {
      return;
    }
  }
  /* Insertion into a sorted array of three; ties keep the first seen. */
  int pos = count_;
  while (pos > 0 && dist_[pos - 1] > d) {
    pos--;
  }
  if (pos >= 3) {
    return;
  }
  for (int i = (std::min)(count_, 2); i > pos; i--) {
    best_[i] = best_[i - 1];
    dist_[i] = dist_[i - 1];
  }
  best_[pos] = std::string(candidate);
  dist_[pos] = d;
  count_ = (std::min)(count_ + 1, 3);
}

void Suggester::addTo(Status &status) const
{
  for (int i = 0; i < count_; i++) {
    status.suggest(best_[i].c_str());
  }
}

bool isIdentifier(std::string_view s)
{
  if (s.empty()) {
    return false;
  }
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    const bool digit = c >= '0' && c <= '9';
    if (!(alpha || (i > 0 && digit))) {
      return false;
    }
  }
  return true;
}

void appendQuoted(std::string &out, std::string_view s)
{
  out += '"';
  for (char c : s) {
    switch (c) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out += c;
    }
  }
  out += '"';
}

void appendDouble(std::string &out, double v)
{
  char buf[64];
  std::to_chars_result r = std::to_chars(buf, buf + sizeof(buf), v);
  out.append(buf, r.ptr);
}

} // namespace coredcc
