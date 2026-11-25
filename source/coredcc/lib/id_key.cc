#include "coredcc/lib/id_key.h"

#include <bit>

namespace coredcc {

namespace {

struct Sip {
  uint64_t v0, v1, v2, v3;

  void round()
  {
    v0 += v1;
    v1 = std::rotl(v1, 13) ^ v0;
    v0 = std::rotl(v0, 32);
    v2 += v3;
    v3 = std::rotl(v3, 16) ^ v2;
    v0 += v3;
    v3 = std::rotl(v3, 21) ^ v0;
    v2 += v1;
    v1 = std::rotl(v1, 17) ^ v2;
    v2 = std::rotl(v2, 32);
  }
  void word(uint64_t m)
  {
    v3 ^= m;
    round();
    v0 ^= m;
  }
};

} // namespace

BlockIdKey BlockIdHasher::key(const BlockId &id) const
{
  Sip s{seed_.hi ^ 0x736f6d6570736575ull,
        seed_.lo ^ 0x646f72616e646f6dull,
        seed_.hi ^ 0x6c7967656e657261ull,
        seed_.lo ^ 0x7465646279746573ull};
  s.word(id.client.value);
  s.word(id.local.hi);
  s.word(id.local.lo);
  s.word(uint64_t(24) << 56); /* the length of the message */
  s.v2 ^= 0xff;
  for (int i = 0; i < 3; i++) {
    s.round();
  }
  return BlockIdKey{id, s.v0 ^ s.v1 ^ s.v2 ^ s.v3};
}

} // namespace coredcc
