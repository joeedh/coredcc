/* Binding descriptors for lib's value types. Datablock and RefCell expose no members
 * yet: their accessors are bound with the rest of bind/ (P1f). */
#include "coredcc/lib/block.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;
using litestl::alloc::PermanentGuard;

const lbt::Struct<RefCell> *RefCell::defineBindings()
{
  static lbt::Struct<RefCell> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<RefCell>("coredcc::RefCell", sizeof(RefCell));
    BIND_STRUCT_MEMBER(st, target);
  }
  return st;
}

const lbt::Struct<RefVisitor> *RefVisitor::defineBindings()
{
  static lbt::Struct<RefVisitor> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<RefVisitor>("coredcc::RefVisitor", sizeof(RefVisitor));
    BIND_STRUCT_METHOD(st, visit, MARGS("cell"));
  }
  return st;
}

const lbt::Struct<QueryAnswer> *QueryAnswer::defineBindings()
{
  static lbt::Struct<QueryAnswer> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<QueryAnswer>("coredcc::QueryAnswer", sizeof(QueryAnswer));
    BIND_STRUCT_MEMBER(st, implType);
    BIND_STRUCT_MEMBER(st, self);
    ::litestl::binding::BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

const lbt::Struct<Datablock> *Datablock::defineBindings()
{
  static lbt::Struct<Datablock> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<Datablock>("coredcc::Datablock", sizeof(Datablock));
  }
  return st;
}

} // namespace coredcc
