#pragma once

/** \file
 * Layouts as text: a subset of XML (architecture §12).
 *
 *     <layout path="objects.active">
 *       <panel label="Transform">
 *         <prop path="location"/>
 *         <row visibleIf="!locked">
 *           <tool invocation="object.clearLocation()">Clear</tool>
 *         </row>
 *       </panel>
 *     </layout>
 *
 * - Elements are named as layoutKindName says, and `sep` is a separator. Attributes are
 * named as layoutAttrName says, or start with `data-`. Others are errors, except on a
 * `custom` and in one, where an element of another name is a Custom with that tag, and in
 * the text of a newer version, where they are skipped (see Layout).
 * - Text is allowed in `label` and `tool`, where it is the label, and in `custom`. Space
 * around it is dropped; in `label` and `tool`, space inside it becomes one blank.
 * - Comments, the five entities, a byte order mark and an `<?xml?>` declaration are
 * allowed. DOCTYPE, CDATA, numeric references, namespaces and processing instructions
 * aren't.
 */

#include "coredcc/layout/layout.h"

#include <cstddef>
#include <string_view>

namespace coredcc {

/** Where a layout's text is wrong. */
struct LayoutPosition {
  /** In bytes. At most the text's size. */
  size_t offset = 0;
  /** From 1. A column is a character. */
  int line = 1;
  int column = 1;
};

/** The position of `offset` in the text, found from `from`, one at or before it. */
LayoutPosition
layoutPosition(std::string_view xml, size_t offset, const LayoutPosition &from = {});

/**
 * The layout in `xml`, within LayoutLimits. Errors are "layout 3:14: what is wrong", with
 * suggestions for misspelled names; `where`, if given, receives the position.
 * Time and memory are linear in the text's size.
 */
Result<Layout> parseLayout(std::string_view xml, LayoutPosition *where = nullptr);

} // namespace coredcc
