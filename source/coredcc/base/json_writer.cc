#include "coredcc/base/json_writer.h"

#include "coredcc/base/assert.h"
#include "coredcc/base/text.h"
#include "coredcc/base/utf8.h"

#include <cmath>
#include <cstdio>
#include <utility>

namespace coredcc {

namespace {
constexpr int64_t kMaxExact = int64_t(1) << 53;
}

/* Before any value: the comma, and the checks. */
void JsonWriter::value()
{
  CDCC_CHECK(!done_, "JsonWriter: a second top-level value");
  if (levels_.empty()) {
    done_ = true;
    return;
  }
  Level &l = levels_.back();
  if (l.object) {
    CDCC_CHECK(keyed_, "JsonWriter: a value in an object without a key");
    keyed_ = false;
    return;
  }
  if (!l.first) {
    out_ += ',';
  }
  l.first = false;
}

void JsonWriter::open(char c)
{
  value();
  done_ = false;
  out_ += c;
  levels_.push_back(Level{c == '{', true});
}

void JsonWriter::close(char c, bool object)
{
  CDCC_CHECK(!levels_.empty() && levels_.back().object == object && !keyed_,
             "JsonWriter: the wrong end");
  levels_.pop_back();
  out_ += c;
  done_ = levels_.empty();
}

JsonWriter &JsonWriter::beginObject()
{
  open('{');
  return *this;
}
JsonWriter &JsonWriter::endObject()
{
  close('}', true);
  return *this;
}
JsonWriter &JsonWriter::beginArray()
{
  open('[');
  return *this;
}
JsonWriter &JsonWriter::endArray()
{
  close(']', false);
  return *this;
}

JsonWriter &JsonWriter::key(std::string_view name)
{
  CDCC_CHECK(!levels_.empty() && levels_.back().object && !keyed_,
             "JsonWriter: a key outside an object, or two in a row");
  if (!levels_.back().first) {
    out_ += ',';
  }
  levels_.back().first = false;
  quoted(name);
  out_ += ':';
  keyed_ = true;
  return *this;
}

void JsonWriter::quoted(std::string_view s)
{
  out_ += '"';
  for (size_t i = 0; i < s.size();) {
    const size_t n = utf8SequenceLength(s, i);
    const char c = s[i];
    if (n == 0) {
      out_ += "\xEF\xBF\xBD";
      i++;
      continue;
    }
    if (c == '"' || c == '\\') {
      out_ += '\\';
      out_ += c;
    }
    else if (uint8_t(c) < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
      out_ += buf;
    }
    else {
      out_.append(s.substr(i, n));
    }
    i += n;
  }
  out_ += '"';
}

JsonWriter &JsonWriter::string(std::string_view s)
{
  value();
  quoted(s);
  return *this;
}

JsonWriter &JsonWriter::boolean(bool v)
{
  value();
  out_ += v ? "true" : "false";
  return *this;
}

JsonWriter &JsonWriter::integer(int64_t v)
{
  if (v < -kMaxExact || v > kMaxExact) {
    return string(std::to_string(v));
  }
  value();
  out_ += std::to_string(v);
  return *this;
}

JsonWriter &JsonWriter::unsignedInteger(uint64_t v)
{
  return v > uint64_t(kMaxExact) ? string(std::to_string(v)) : integer(int64_t(v));
}

JsonWriter &JsonWriter::number(double v)
{
  std::string text;
  appendDouble(text, v);
  if (!std::isfinite(v)) {
    return string(text);
  }
  value();
  out_ += text;
  return *this;
}

JsonWriter &JsonWriter::null()
{
  value();
  out_ += "null";
  return *this;
}

std::string JsonWriter::take()
{
  CDCC_CHECK(done_, "JsonWriter: the text isn't finished");
  done_ = false;
  return std::exchange(out_, std::string());
}

} // namespace coredcc
