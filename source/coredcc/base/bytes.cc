#include "coredcc/base/bytes.h"

#include "coredcc/base/assert.h"

#include <bit>
#include <cstring>
#include <string>
#include <utility>

namespace coredcc {

using litestl::util::Vector;

/* ---- BytesWriter ---- */

uint8_t *BytesWriter::grow(size_t n)
{
  const size_t size = size_t(data_.size());
  if (!status_.ok()) {
    return nullptr;
  }
  if (n > max_ || size > max_ - n) {
    fail(Status::error(StatusCode::OutOfRange,
                       ("more than " + std::to_string(max_) + " bytes to write").c_str()));
    return nullptr;
  }
  data_.resize<false>(size + n);
  return data_.data() + size;
}

template <class T> void BytesWriter::number(T v)
{
  if (uint8_t *p = grow(sizeof(T))) {
    for (size_t i = 0; i < sizeof(T); i++) {
      p[i] = uint8_t(v >> (8 * i));
    }
  }
}

void BytesWriter::u8(uint8_t v)
{
  number(v);
}
void BytesWriter::u16(uint16_t v)
{
  number(v);
}
void BytesWriter::u32(uint32_t v)
{
  number(v);
}
void BytesWriter::u64(uint64_t v)
{
  number(v);
}
void BytesWriter::i64(int64_t v)
{
  number(uint64_t(v));
}
void BytesWriter::f64(double v)
{
  number(std::bit_cast<uint64_t>(v));
}
void BytesWriter::boolean(bool v)
{
  number(uint8_t(v ? 1 : 0));
}

void BytesWriter::str(std::string_view s)
{
  s = s.substr(0, s.find('\0'));
  blob(std::span(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
}

void BytesWriter::blob(std::span<const uint8_t> bytes)
{
  if (bytes.size() > UINT32_MAX - 4) {
    fail(Status::error(StatusCode::OutOfRange, "more than 4 GiB to write"));
    return;
  }
  if (uint8_t *p = grow(4 + bytes.size())) {
    const uint32_t n = uint32_t(bytes.size());
    for (int i = 0; i < 4; i++) {
      p[i] = uint8_t(n >> (8 * i));
    }
    if (n > 0) {
      memcpy(p + 4, bytes.data(), n);
    }
  }
}

void BytesWriter::raw(std::span<const uint8_t> bytes)
{
  if (uint8_t *p = grow(bytes.size()); p && !bytes.empty()) {
    memcpy(p, bytes.data(), bytes.size());
  }
}

size_t BytesWriter::beginSection()
{
  const size_t token = size_t(data_.size());
  u32(0);
  return token;
}

void BytesWriter::endSection(size_t token)
{
  if (!status_.ok()) {
    return;
  }
  CDCC_CHECK(token + 4 <= size_t(data_.size()), "endSection without its beginSection");
  const uint32_t n = uint32_t(size_t(data_.size()) - token - 4);
  for (int i = 0; i < 4; i++) {
    data_[int(token) + i] = uint8_t(n >> (8 * i));
  }
}

void BytesWriter::fail(Status s)
{
  if (status_.ok()) {
    status_ = std::move(s);
  }
}

Vector<uint8_t> BytesWriter::take()
{
  Vector<uint8_t> out = std::move(data_);
  data_ = Vector<uint8_t>();
  status_ = Status();
  return out;
}

/* ---- BytesReader ---- */

const uint8_t *BytesReader::take(size_t n)
{
  if (!status_.ok()) {
    return nullptr;
  }
  if (n > remaining()) {
    fail(StatusCode::InvalidArgument, "the data ends too early");
    return nullptr;
  }
  const uint8_t *p = bytes_.data() + at_;
  at_ += n;
  return p;
}

template <class T> T BytesReader::number()
{
  T v = 0;
  if (const uint8_t *p = take(sizeof(T))) {
    for (size_t i = 0; i < sizeof(T); i++) {
      v = T(v | T(T(p[i]) << (8 * i)));
    }
  }
  return v;
}

uint8_t BytesReader::u8()
{
  return number<uint8_t>();
}
uint16_t BytesReader::u16()
{
  return number<uint16_t>();
}
uint32_t BytesReader::u32()
{
  return number<uint32_t>();
}
uint64_t BytesReader::u64()
{
  return number<uint64_t>();
}
int64_t BytesReader::i64()
{
  return int64_t(number<uint64_t>());
}
double BytesReader::f64()
{
  return std::bit_cast<double>(number<uint64_t>());
}

bool BytesReader::boolean()
{
  const uint8_t v = u8();
  if (v > 1) {
    fail(StatusCode::InvalidArgument, "a boolean that is neither 0 nor 1");
  }
  return v == 1;
}

std::string_view BytesReader::str()
{
  const std::span<const uint8_t> b = blob();
  if (b.empty()) {
    return {};
  }
  if (memchr(b.data(), 0, b.size())) {
    fail(StatusCode::InvalidArgument, "text with a NUL in it");
    return {};
  }
  return std::string_view(reinterpret_cast<const char *>(b.data()), b.size());
}

std::span<const uint8_t> BytesReader::blob()
{
  return raw(u32());
}

std::span<const uint8_t> BytesReader::raw(size_t n)
{
  const uint8_t *p = take(n);
  return p ? std::span(p, n) : std::span<const uint8_t>();
}

uint32_t BytesReader::count(size_t minItemBytes)
{
  CDCC_CHECK(minItemBytes > 0, "count() needs the least size of an item");
  const uint32_t n = u32();
  if (n > remaining() / minItemBytes) {
    fail(StatusCode::InvalidArgument, "a count of more items than the data holds");
    return 0;
  }
  return n;
}

void BytesReader::fail(Status s)
{
  if (!s.ok()) {
    fail(s.code(), s.message());
  }
}

void BytesReader::fail(StatusCode code, const char *message)
{
  if (!status_.ok()) {
    return;
  }
  status_ = Status::error(code,
                          (std::string(message) + " (at byte " + std::to_string(at_) + ")").c_str());
  at_ = bytes_.size();
}

} // namespace coredcc
