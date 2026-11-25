#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace coredcc {

/**
 * Builds JSON text, without whitespace. Calls must nest properly, and inside an object each
 * value follows a key() (checked: a mistake aborts). The output is valid JSON and UTF-8 for
 * any strings: bytes that aren't UTF-8 become U+FFFD.
 */
class JsonWriter {
 public:
  JsonWriter &beginObject();
  JsonWriter &endObject();
  JsonWriter &beginArray();
  JsonWriter &endArray();
  JsonWriter &key(std::string_view name);

  JsonWriter &string(std::string_view s);
  JsonWriter &boolean(bool v);
  /** A string past 2^53 in size, where a JSON reader's double would round it. */
  JsonWriter &integer(int64_t v);
  JsonWriter &unsignedInteger(uint64_t v);
  /** NaN and the infinities as the strings "nan", "inf" and "-inf". */
  JsonWriter &number(double v);
  JsonWriter &null();

  /** The text, once everything has been ended. */
  std::string take();

 private:
  void value();
  void open(char c);
  void close(char c, bool object);
  void quoted(std::string_view s);

  struct Level {
    bool object = false;
    bool first = true;
  };
  std::string out_;
  std::vector<Level> levels_;
  bool keyed_ = false;
  bool done_ = false;
};

} // namespace coredcc
