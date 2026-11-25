#pragma once

/** \file
 * What the parser and the builder fill in, and the checks they share. Not public API.
 */

#include "coredcc/layout/layout.h"

#include <string>
#include <string_view>
#include <vector>

namespace coredcc {

struct DraftAttr {
  LayoutAttrId id = LayoutAttrId::Free;
  std::string name;
  std::string value;
  /** In the text: the attribute, and its value. */
  size_t offset = 0;
  size_t valueOffset = 0;
  /** The value is in the text as it is, so an offset in the one is an offset in the other. */
  bool verbatim = false;
};

struct DraftNode {
  LayoutKind kind = LayoutKind::Layout;
  int parent = -1;
  std::vector<DraftAttr> attrs;
  size_t offset = 0;

  DraftAttr *find(LayoutAttrId id, std::string_view name = {});
};

/** An error, and where. */
struct DraftFault {
  Status status;
  int node = -1;
  size_t offset = 0;
};

/** Turns drafts into a Layout. A friend of it. */
struct LayoutAssembler {
  /** Checks everything that Layout promises. Parents come before children in `nodes`, and
   * node 0 is the root. */
  static Result<Layout> assemble(std::vector<DraftNode> nodes, DraftFault &fault);
};

namespace layout_detail {

/** The kind of an element's name, `sep` included. */
bool isKindName(std::string_view name, LayoutKind &out);
/** All kinds' names, for suggestions. */
std::vector<std::string_view> kindNames();
bool mayHave(LayoutKind kind, LayoutAttrId id);
/** May hold elements. */
bool isContainer(LayoutKind kind);
/** `name` as an attribute of `kind`: Free for a Custom's own, else an error. */
Result<LayoutAttrId> attrOf(LayoutKind kind, std::string_view name);
/** [A-Za-z_], and [0-9.-] if it isn't the first. */
bool isXmlNameChar(char c, bool first);
bool isXmlName(std::string_view s);
/** The first byte that is a control character (but tab and line ends), U+FFFE, U+FFFF or not
 * UTF-8; s.size() if there is none. `what` says what it is. */
size_t firstBadByte(std::string_view s, const char *&what);
/** joinLayoutPath as text. `prefix` is canonical text. */
Result<std::string> joinPathText(const std::string &prefix,
                                 std::string_view path,
                                 size_t *errorOffset = nullptr);

} // namespace layout_detail
} // namespace coredcc
