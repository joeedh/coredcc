/* Binding descriptors for base's value types. Kept out of the headers: bound members
 * compile slowly (architecture §2a, build time). */
#include "coredcc/base/block_id.h"
#include "coredcc/base/id.h"
#include "coredcc/base/status.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::alloc::PermanentGuard;

namespace {
constexpr const char *idStructName(IdDomain d)
{
  switch (d) {
    case IdDomain::Type:
      return "coredcc::TypeId";
    case IdDomain::Interface:
      return "coredcc::InterfaceId";
    case IdDomain::Client:
      return "coredcc::ClientId";
    case IdDomain::Struct:
      return "coredcc::StructId";
    case IdDomain::Command:
      return "coredcc::CommandId";
  }
  return "coredcc::Id";
}
} // namespace

template <IdDomain D> const lbt::Struct<Id<D>> *Id<D>::defineBindings()
{
  static lbt::Struct<Id<D>> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<Id<D>>(idStructName(D), sizeof(Id<D>));
    BIND_STRUCT_MEMBER(st, value);
  }
  return st;
}

template struct Id<IdDomain::Type>;
template struct Id<IdDomain::Interface>;
template struct Id<IdDomain::Client>;
template struct Id<IdDomain::Struct>;
template struct Id<IdDomain::Command>;

const lbt::Struct<Status> *Status::defineBindings()
{
  static lbt::Struct<Status> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<Status>("coredcc::Status", sizeof(Status));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, ok, MARGS());
    BIND_STRUCT_METHOD(st, message, MARGS());
  }
  return st;
}

const lbt::Struct<LocalId> *LocalId::defineBindings()
{
  static lbt::Struct<LocalId> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<LocalId>("coredcc::LocalId", sizeof(LocalId));
    BIND_STRUCT_MEMBER(st, hi);
    BIND_STRUCT_MEMBER(st, lo);
  }
  return st;
}

const lbt::Struct<BlockId> *BlockId::defineBindings()
{
  static lbt::Struct<BlockId> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<BlockId>("coredcc::BlockId", sizeof(BlockId));
    BIND_STRUCT_MEMBER(st, client);
    BIND_STRUCT_MEMBER(st, local);
  }
  return st;
}

} // namespace coredcc
