#pragma once

/** \file
 * A small text form of input events, so that a scenario is written once for the C++
 * tests, the test app and the Unreal spike.
 *
 * Statements are separated by `;` or new lines; `#` starts a comment to the end of the
 * line. Positions are device pixels.
 *
 * | Statement | Events |
 * |---|---|
 * | `down X Y [left|middle|right]` | PointerDown |
 * | `move X Y` | PointerMove |
 * | `up X Y [left|middle|right]` | PointerUp |
 * | `drag X0 Y0 X1 Y1 [steps=N]` | down, N moves (default 4) evenly to the end, up |
 * | `wheel DX DY` | Wheel, at the last pointer position |
 * | `key NAME` | KeyDown, then KeyUp |
 * | `keydown NAME`, `keyup NAME` | one of them |
 * | `cancel`, `focuslost` | Cancel, FocusLost |
 *
 * After a statement's arguments come options, which apply to its events:
 * - modifiers: `shift`, `ctrl`, `alt`, `meta`;
 * - `pen` or `touch` (pointer type), `pressure=P`, `tiltx=D`, `tilty=D`, `time=S`.
 *
 * Key names are a single printable character (`x`, `1`, `/`), or `escape`, `enter`,
 * `tab`, `backspace`, `delete`, `left`, `right`, `up`, `down`, `home`, `end`, `pageup`,
 * `pagedown`, `shift`, `ctrl`, `alt`, `meta` and `f1` to `f12`.
 *
 * Held buttons (`InputEvent::buttons`) follow the downs and ups. Without `time=`, each
 * event is 1/60 s after the one before.
 */

#include "coredcc/base/status.h"
#include "coredcc/tool/input.h"

#include "litestl/util/vector.h"

#include <span>
#include <string_view>

namespace coredcc {

class Context;
class ToolManager;
enum class EventResult : int32_t; /* tool.h */
struct View;

/** The events of `text`. InvalidArgument names the byte offset of the first mistake. */
Result<litestl::util::Vector<InputEvent>> parseEventScript(std::string_view text);

/** Dispatches each event to `tm`. Stops at the first error, which names the event's
 * index. `handled`, if given, gets one EventResult per event dispatched. */
Status playEvents(ToolManager &tm,
                  Context &ctx,
                  const View &view,
                  std::span<const InputEvent> events,
                  litestl::util::Vector<EventResult> *handled = nullptr);

/** parseEventScript, then playEvents. */
Status
playEventScript(ToolManager &tm, Context &ctx, const View &view, std::string_view text);

} // namespace coredcc
