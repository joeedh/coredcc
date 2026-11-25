/* Binding descriptors for the transform types. */
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/pick.h"

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
    BIND_STRUCT_MEMBER(st, member);
  }
  return st;
}

lbt::Struct<PickQuery> *PickQuery::defineBindings()
{
  static lbt::Struct<PickQuery> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickQuery>("coredcc::PickQuery", sizeof(PickQuery));
    BIND_STRUCT_METHOD(st, planeCount, MARGS());
    BIND_STRUCT_METHOD(st, planeNormal, MARGS("i"));
    BIND_STRUCT_METHOD(st, planeOffset, MARGS("i"));
    BIND_STRUCT_METHOD(st, hasRay, MARGS());
    BIND_STRUCT_METHOD(st, rayOrigin, MARGS());
    BIND_STRUCT_METHOD(st, rayDirection, MARGS());
    BIND_STRUCT_METHOD(st, radiusPixels, MARGS());
  }
  return st;
}

lbt::Struct<PickSink> *PickSink::defineBindings()
{
  static lbt::Struct<PickSink> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PickSink>("coredcc::PickSink", sizeof(PickSink));
    BIND_STRUCT_METHOD(st, element, MARGS("domain", "element"));
    BIND_STRUCT_METHOD(st, point, MARGS("p"));
    BIND_STRUCT_METHOD(st, segment, MARGS("a", "b"));
    BIND_STRUCT_METHOD(st, triangle, MARGS("a", "b", "c"));
    BIND_STRUCT_METHOD(st, sphere, MARGS("center", "radius"));
    BIND_STRUCT_METHOD(st, capsule, MARGS("a", "b", "radius"));
    BIND_STRUCT_METHOD(st, box, MARGS("box"));
    BIND_STRUCT_METHOD(st, any, MARGS());
  }
  return st;
}

} // namespace coredcc
