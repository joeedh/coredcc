/* Binding descriptors for the tool types. */
#include "coredcc/tool/manager.h"
#include "coredcc/tool/tool.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

namespace coredcc {

namespace lbt = litestl::binding::types;

lbt::Struct<PointerSample> *PointerSample::defineBindings()
{
  static lbt::Struct<PointerSample> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<PointerSample>("coredcc::PointerSample", sizeof(PointerSample));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, x);
    BIND_STRUCT_MEMBER(st, y);
    BIND_STRUCT_MEMBER(st, pressure);
    BIND_STRUCT_MEMBER(st, tiltX);
    BIND_STRUCT_MEMBER(st, tiltY);
    BIND_STRUCT_MEMBER(st, twist);
    BIND_STRUCT_MEMBER(st, time);
  }
  return st;
}

lbt::Struct<InputEvent> *InputEvent::defineBindings()
{
  static lbt::Struct<InputEvent> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<InputEvent>("coredcc::InputEvent", sizeof(InputEvent));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, x);
    BIND_STRUCT_MEMBER(st, y);
    BIND_STRUCT_MEMBER(st, button);
    BIND_STRUCT_MEMBER(st, buttons);
    BIND_STRUCT_MEMBER(st, pointerId);
    BIND_STRUCT_MEMBER(st, pressure);
    BIND_STRUCT_MEMBER(st, tiltX);
    BIND_STRUCT_MEMBER(st, tiltY);
    BIND_STRUCT_MEMBER(st, twist);
    BIND_STRUCT_MEMBER(st, wheelX);
    BIND_STRUCT_MEMBER(st, wheelY);
    BIND_STRUCT_MEMBER(st, key);
    BIND_STRUCT_MEMBER(st, repeat);
    BIND_STRUCT_MEMBER(st, modifiers);
    BIND_STRUCT_MEMBER(st, time);
    BIND_STRUCT_METHOD(st, typeValue, MARGS());
    BIND_STRUCT_METHOD(st, setType, MARGS("type"));
    BIND_STRUCT_METHOD(st, pointerValue, MARGS());
    BIND_STRUCT_METHOD(st, setPointer, MARGS("pointer"));
    BIND_STRUCT_METHOD(st, has, MARGS("modifier"));
    BIND_STRUCT_METHOD(st, isPointer, MARGS());
    BIND_STRUCT_METHOD(st, sampleCount, MARGS());
    BIND_STRUCT_METHOD(st, sampleAt, MARGS("i"));
    BIND_STRUCT_METHOD(st, addSample, MARGS("s"));
  }
  return st;
}

lbt::Struct<GizmoHit> *GizmoHit::defineBindings()
{
  static lbt::Struct<GizmoHit> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<GizmoHit>("coredcc::GizmoHit", sizeof(GizmoHit));
    BIND_STRUCT_DEFAULT_CONSTRUCTOR(st);
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
    BIND_STRUCT_MEMBER(st, part);
    BIND_STRUCT_MEMBER(st, distance);
    BIND_STRUCT_MEMBER(st, depth);
  }
  return st;
}

const lbt::Struct<ToolDef> *ToolDef::defineBindings()
{
  static lbt::Struct<ToolDef> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ToolDef>("coredcc::ToolDef", sizeof(ToolDef));
  }
  return st;
}

} // namespace coredcc
