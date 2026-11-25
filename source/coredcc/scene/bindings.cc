/* Binding descriptors for the transform types. */
#include "coredcc/scene/transform.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::SetWorldMode>::bind()
{
  using coredcc::SetWorldMode;
  static types::Enum *e = nullptr;
  if (!e) {
    alloc::PermanentGuard guard;
    e = new types::Enum("coredcc::SetWorldMode", sizeof(SetWorldMode));
    e->addItem("Exact", int(SetWorldMode::Exact));
    e->addItem("Closest", int(SetWorldMode::Closest));
  }
  return e;
}

namespace coredcc {

namespace lbt = litestl::binding::types;

lbt::Struct<ChannelBlock> *ChannelBlock::defineBindings()
{
  static lbt::Struct<ChannelBlock> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ChannelBlock>("coredcc::ChannelBlock", sizeof(ChannelBlock));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_STRUCT_METHOD(st, get, MARGS("i"));
    BIND_STRUCT_METHOD(st, set, MARGS("i", "v"));
  }
  return st;
}

lbt::Struct<EvalCtx> *EvalCtx::defineBindings()
{
  static lbt::Struct<EvalCtx> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<EvalCtx>("coredcc::EvalCtx", sizeof(EvalCtx));
    BIND_STRUCT_METHOD(st, channelCount, MARGS());
    BIND_STRUCT_METHOD(st, channel, MARGS("i"));
    BIND_STRUCT_METHOD(st, boolChannel, MARGS("i"));
    BIND_STRUCT_METHOD(st, enumChannel, MARGS("i", "count"));
    BIND_STRUCT_METHOD(st, time, MARGS());
  }
  return st;
}

lbt::Struct<TransformState> *TransformState::defineBindings()
{
  static lbt::Struct<TransformState> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformState>("coredcc::TransformState", sizeof(TransformState));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, world);
    BIND_STRUCT_MEMBER(st, rotation);
    BIND_STRUCT_MEMBER(st, localScale);
    BIND_STRUCT_METHOD(st, finite, MARGS());
  }
  return st;
}

lbt::Struct<TransformDelta> *TransformDelta::defineBindings()
{
  static lbt::Struct<TransformDelta> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformDelta>("coredcc::TransformDelta", sizeof(TransformDelta));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, basis);
    BIND_STRUCT_MEMBER(st, pivot);
    BIND_STRUCT_MEMBER(st, vector);
    BIND_STRUCT_MEMBER(st, axis);
    BIND_STRUCT_MEMBER(st, angle);
    BIND_STRUCT_METHOD(st, kindValue, MARGS());
    BIND_STRUCT_METHOD(st, setKind, MARGS("k"));
    BIND_STRUCT_METHOD(st, matrix, MARGS());
    BIND_STRUCT_METHOD(st, rotation, MARGS());
  }
  return st;
}

} // namespace coredcc
