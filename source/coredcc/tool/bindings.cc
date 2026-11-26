/* Binding descriptors for the tool types. */
#include "coredcc/tool/manager.h"
#include "coredcc/tool/tool.h"

#include "coredcc/base/detail/enum_binding.h"

#include "litestl/binding/binding.h"
#include "litestl/util/alloc.h"

using coredcc::detail::bindEnum;

const litestl::binding::BindingBase *litestl::binding::Binder<coredcc::EventType>::bind()
{
  using coredcc::EventType;
  static const BindingBase *e = bindEnum("coredcc::EventType",
                                         sizeof(EventType),
                                         {{"PointerDown", int(EventType::PointerDown)},
                                          {"PointerMove", int(EventType::PointerMove)},
                                          {"PointerUp", int(EventType::PointerUp)},
                                          {"Wheel", int(EventType::Wheel)},
                                          {"KeyDown", int(EventType::KeyDown)},
                                          {"KeyUp", int(EventType::KeyUp)},
                                          {"Cancel", int(EventType::Cancel)},
                                          {"FocusLost", int(EventType::FocusLost)}});
  return e;
}

const litestl::binding::BindingBase *
litestl::binding::Binder<coredcc::PointerType>::bind()
{
  using coredcc::PointerType;
  static const BindingBase *e = bindEnum("coredcc::PointerType",
                                         sizeof(PointerType),
                                         {{"Mouse", int(PointerType::Mouse)},
                                          {"Pen", int(PointerType::Pen)},
                                          {"Touch", int(PointerType::Touch)}});
  return e;
}

const litestl::binding::BindingBase *
litestl::binding::Binder<coredcc::EventResult>::bind()
{
  using coredcc::EventResult;
  static const BindingBase *e = bindEnum("coredcc::EventResult",
                                         sizeof(EventResult),
                                         {{"Unhandled", int(EventResult::Unhandled)},
                                          {"Handled", int(EventResult::Handled)}});
  return e;
}

const litestl::binding::BindingBase *
litestl::binding::Binder<coredcc::ModalResult>::bind()
{
  using coredcc::ModalResult;
  static const BindingBase *e = bindEnum("coredcc::ModalResult",
                                         sizeof(ModalResult),
                                         {{"Continue", int(ModalResult::Continue)},
                                          {"PassThrough", int(ModalResult::PassThrough)},
                                          {"Finish", int(ModalResult::Finish)},
                                          {"Cancel", int(ModalResult::Cancel)}});
  return e;
}

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
    BIND_STRUCT_MEMBER(st, type);
    BIND_STRUCT_MEMBER(st, x);
    BIND_STRUCT_MEMBER(st, y);
    BIND_STRUCT_MEMBER(st, button);
    BIND_STRUCT_MEMBER(st, buttons);
    BIND_STRUCT_MEMBER(st, pointer);
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
    BIND_STRUCT_MEMBER(st, viewDistance);
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
