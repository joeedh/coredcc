/* Binding descriptors for lib's value types. Datablock and RefCell expose no members
 * yet: their accessors are bound with the rest of bind/ (P1f). */
#include "coredcc/lib/block.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/lib/provider.h"

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

const lbt::Struct<BlockWriter> *BlockWriter::defineBindings()
{
  static lbt::Struct<BlockWriter> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<BlockWriter>("coredcc::BlockWriter", sizeof(BlockWriter));
  }
  return st;
}

const lbt::Struct<BlockReader> *BlockReader::defineBindings()
{
  static lbt::Struct<BlockReader> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<BlockReader>("coredcc::BlockReader", sizeof(BlockReader));
  }
  return st;
}

const lbt::Struct<RekeyContext> *RekeyContext::defineBindings()
{
  static lbt::Struct<RekeyContext> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<RekeyContext>("coredcc::RekeyContext", sizeof(RekeyContext));
    BIND_STRUCT_METHOD(st, isFree, MARGS("id"));
  }
  return st;
}

const lbt::Struct<BlockCopy> *BlockCopy::defineBindings()
{
  static lbt::Struct<BlockCopy> *st = nullptr;
  if (!st) {
    PermanentGuard guard;
    st = new lbt::Struct<BlockCopy>("coredcc::BlockCopy", sizeof(BlockCopy));
  }
  return st;
}

} // namespace coredcc
