#pragma once

/** \file
 * InputEvent: what a host hands the ToolManager (architecture §11, "Input events").
 *
 * Positions are device pixels in the space of View::viewport and pickScene, y down. A
 * host with several viewports in one canvas gives canvas coordinates and a View for each
 * viewport. Wheel deltas are logical pixels. Times are seconds, from any monotonic clock.
 */

#include "litestl/binding/binding_bind.h"
#include "litestl/util/vector.h"

#include <cstdint>

namespace litestl::binding::types {
template <typename T> struct Struct;
}

namespace coredcc {

enum class EventType : int32_t {
  PointerDown = 0,
  PointerMove = 1,
  PointerUp = 2,
  Wheel = 3,
  KeyDown = 4,
  KeyUp = 5,
  /** The host wants the gesture in progress to stop (Escape in its own UI, say). */
  Cancel = 6,
  /** The window or the viewport lost the input focus. */
  FocusLost = 7,
};

enum class PointerType : int32_t { Mouse = 0, Pen = 1, Touch = 2 };

/** Bits of InputEvent::modifiers. */
namespace modifiers {
inline constexpr uint32_t kShift = 1;
inline constexpr uint32_t kCtrl = 2;
inline constexpr uint32_t kAlt = 4;
inline constexpr uint32_t kMeta = 8;
} // namespace modifiers

/**
 * Keys. A printable key is the Unicode code point it gives without Shift ('x', '1',
 * '/'); the others are these, above Unicode's range. `docs/embedding.md` maps DOM's
 * `KeyboardEvent.key` and Unreal's FKey names to them.
 */
namespace keys {
inline constexpr int32_t kEscape = 0x110001;
inline constexpr int32_t kEnter = 0x110002;
inline constexpr int32_t kTab = 0x110003;
inline constexpr int32_t kBackspace = 0x110004;
inline constexpr int32_t kDelete = 0x110005;
inline constexpr int32_t kLeft = 0x110006;
inline constexpr int32_t kRight = 0x110007;
inline constexpr int32_t kUp = 0x110008;
inline constexpr int32_t kDown = 0x110009;
inline constexpr int32_t kHome = 0x11000a;
inline constexpr int32_t kEnd = 0x11000b;
inline constexpr int32_t kPageUp = 0x11000c;
inline constexpr int32_t kPageDown = 0x11000d;
inline constexpr int32_t kShift = 0x11000e;
inline constexpr int32_t kCtrl = 0x11000f;
inline constexpr int32_t kAlt = 0x110010;
inline constexpr int32_t kMeta = 0x110011;
/** F1 is kF1, F12 is kF1 + 11. */
inline constexpr int32_t kF1 = 0x110020;
} // namespace keys

/** A pointer position that the host coalesced into a move. */
struct PointerSample {
  double x = 0.0, y = 0.0;
  double pressure = 0.5;
  double tiltX = 0.0, tiltY = 0.0, twist = 0.0;
  double time = 0.0;

  static litestl::binding::types::Struct<PointerSample> *defineBindings();
};

struct InputEvent {
  EventType type = EventType::PointerMove;
  double x = 0.0, y = 0.0;
  /** The button that went down or up, in DOM's numbering: 0 left, 1 middle, 2 right. */
  int32_t button = 0;
  /** The buttons held, as bits: 1 left, 2 right, 4 middle (DOM's `buttons`). */
  uint32_t buttons = 0;
  PointerType pointer = PointerType::Mouse;
  int32_t pointerId = 0;
  /** 0 to 1; 0.5 for a mouse with a button down, as DOM does. */
  double pressure = 0.5;
  /** Degrees, -90 to 90; twist 0 to 359. */
  double tiltX = 0.0, tiltY = 0.0, twist = 0.0;
  double wheelX = 0.0, wheelY = 0.0;
  /** A key (see `keys`) for KeyDown and KeyUp. */
  int32_t key = 0;
  bool repeat = false;
  uint32_t modifiers = 0;
  double time = 0.0;

  bool has(uint32_t modifier) const
  {
    return (modifiers & modifier) != 0;
  }
  bool isPointer() const
  {
    return type == EventType::PointerDown || type == EventType::PointerMove ||
           type == EventType::PointerUp;
  }
  /** Whether `type` and `pointer` are values of their enums (scripts can set any
   * number). ToolManager::dispatch refuses an event that isn't. */
  bool valid() const
  {
    return int32_t(type) >= 0 && int32_t(type) <= int32_t(EventType::FocusLost) &&
           int32_t(pointer) >= 0 && int32_t(pointer) <= int32_t(PointerType::Touch);
  }

  /* Moves: the positions that the host coalesced into this one, oldest first. The
   * event's own position is the newest and isn't repeated here. */
  int32_t sampleCount() const
  {
    return int32_t(samples_.size());
  }
  /** An empty sample out of range. */
  PointerSample sampleAt(int32_t i) const
  {
    return i >= 0 && i < sampleCount() ? samples_[i] : PointerSample();
  }
  void addSample(const PointerSample &s)
  {
    samples_.append(s);
  }

  static litestl::binding::types::Struct<InputEvent> *defineBindings();

  /** Public, so that the struct is standard-layout (its members are bound by offset);
   * scripts use the methods. One inline, not 0: litestl keeps them in an array, and a
   * zero-size array is an extension that MSVC refuses in a std::span of these. */
  litestl::util::Vector<PointerSample, 1> samples_;
};

} // namespace coredcc

namespace litestl::binding {
/** EventType and PointerType, bound as enums (tool/bindings.cc). */
template <> struct Binder<coredcc::EventType> {
  static const BindingBase *bind();
};
template <> struct Binder<coredcc::PointerType> {
  static const BindingBase *bind();
};
} // namespace litestl::binding
