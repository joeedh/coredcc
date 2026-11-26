/* Binding descriptors for the transform types. */
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/transform_registry.h"

#include "coredcc/base/detail/enum_binding.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

const litestl::binding::BindingBase *
litestl::binding::Binder<coredcc::SetWorldMode>::bind()
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

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::EvalStatus>::bind()
{
  using coredcc::EvalStatus;
  static types::Enum *e = nullptr;
  if (!e) {
    alloc::PermanentGuard guard;
    e = new types::Enum("coredcc::EvalStatus", sizeof(EvalStatus));
    e->addItem("Ok", int(EvalStatus::Ok));
    e->addItem("Invalid", int(EvalStatus::Invalid));
    e->addItem("ModelFailed", int(EvalStatus::ModelFailed));
    e->addItem("MissingModel", int(EvalStatus::MissingModel));
    e->addItem("Cycle", int(EvalStatus::Cycle));
    e->addItem("NoTransform", int(EvalStatus::NoTransform));
  }
  return e;
}

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::DeltaKind>::bind()
{
  using coredcc::DeltaKind;
  static const BindingBase *e =
      coredcc::detail::bindEnum("coredcc::DeltaKind",
                                sizeof(DeltaKind),
                                {{"Translate", int(DeltaKind::Translate)},
                                 {"Rotate", int(DeltaKind::Rotate)},
                                 {"Scale", int(DeltaKind::Scale)}});
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
    BIND_STRUCT_METHOD(st, resize, MARGS("size"));
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
    BIND_STRUCT_METHOD(st, isFixed, MARGS("i"));
  }
  return st;
}

lbt::Struct<TransformState> *TransformState::defineBindings()
{
  static lbt::Struct<TransformState> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformState>("coredcc::TransformState",
                                         sizeof(TransformState));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, world);
    BIND_STRUCT_MEMBER(st, rotation);
    BIND_STRUCT_MEMBER(st, localScale);
    BIND_STRUCT_METHOD(st, finite, MARGS());
  }
  return st;
}

lbt::Struct<ChannelEntries> *ChannelEntries::defineBindings()
{
  static lbt::Struct<ChannelEntries> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ChannelEntries>("coredcc::ChannelEntries",
                                         sizeof(ChannelEntries));
    BIND_STRUCT_METHOD(st, get, MARGS("name"));
    BIND_STRUCT_METHOD(st, set, MARGS("name", "value"));
    BIND_STRUCT_METHOD(st, remove, MARGS("name"));
    BIND_STRUCT_METHOD(st, rename, MARGS("from", "to"));
  }
  return st;
}

lbt::Struct<TransformTypeInfo> *TransformTypeInfo::defineBindings()
{
  static lbt::Struct<TransformTypeInfo> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformTypeInfo>("coredcc::TransformTypeInfo",
                                            sizeof(TransformTypeInfo));
  }
  return st;
}

lbt::Struct<TransformDelta> *TransformDelta::defineBindings()
{
  static lbt::Struct<TransformDelta> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<TransformDelta>("coredcc::TransformDelta",
                                         sizeof(TransformDelta));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, basis);
    BIND_STRUCT_MEMBER(st, pivot);
    BIND_STRUCT_MEMBER(st, vector);
    BIND_STRUCT_MEMBER(st, axis);
    BIND_STRUCT_MEMBER(st, angle);
    BIND_STRUCT_MEMBER(st, kind);
    BIND_STRUCT_METHOD(st, matrix, MARGS());
    BIND_STRUCT_METHOD(st, rotation, MARGS());
  }
  return st;
}

lbt::Struct<ObjectInfo> *ObjectInfo::defineBindings()
{
  static lbt::Struct<ObjectInfo> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ObjectInfo>("coredcc::ObjectInfo", sizeof(ObjectInfo));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, transformType);
    BIND_STRUCT_MEMBER(st, parent);
    BIND_STRUCT_MEMBER(st, data);
    BIND_STRUCT_MEMBER(st, flags);
    BIND_STRUCT_MEMBER(st, locks);
  }
  return st;
}

lbt::Struct<ObjectEval> *ObjectEval::defineBindings()
{
  static lbt::Struct<ObjectEval> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ObjectEval>("coredcc::ObjectEval", sizeof(ObjectEval));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, state);
    BIND_STRUCT_MEMBER(st, localBounds);
    BIND_STRUCT_MEMBER(st, bounds);
    BIND_STRUCT_MEMBER(st, status);
    BIND_STRUCT_MEMBER(st, flags);
    BIND_STRUCT_MEMBER(st, parent);
    BIND_STRUCT_MEMBER(st, member);
  }
  return st;
}

} // namespace coredcc
