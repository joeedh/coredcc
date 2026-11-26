#pragma once

/** \file
 * ToolSettingsStore: the values of tools' settings, by path
 * `toolSettings["core.tool.transform"].mode`.
 */

#include "coredcc/api/struct.h"
#include "coredcc/props/bag.h"
#include "coredcc/tool/tool.h"

#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <string>
#include <string_view>

namespace coredcc {

/**
 * The settings of every registered tool, one bag per ToolDef, made on first use with the
 * defs' defaults. A Runtime extension.
 *
 * - Not undoable: by path they are NoUndo, as commandDefaults are. Watchers see changes
 *   (the ChangeBus's epoch moves).
 * - `version` moves with each change of a tool's values, so the ToolManager can tell its
 *   tool.
 * - `serialize` writes one invocation-like line per tool, `core.tool.transform(mode=
 *   'ROTATE')`; `load` reads it back and skips tools and settings that are gone.
 */
class ToolSettingsStore {
public:
  static constexpr const char *kExtName = "coredcc.ToolSettingsStore";
  static constexpr const char *kMember = "toolSettings";

  explicit ToolSettingsStore(Runtime *rt);
  ~ToolSettingsStore();
  ToolSettingsStore(const ToolSettingsStore &) = delete;
  ToolSettingsStore &operator=(const ToolSettingsStore &) = delete;

  /** The tool's values. */
  const PropertyBag &settings(const ToolDef &def);
  /** One value; its def's default until it is set. Null for no such setting. */
  const PropValue *get(const ToolDef &def, std::string_view name);
  /** Coerced to the setting's def. NotFound for no such setting. */
  Status set(const ToolDef &def, std::string_view name, const PropValue &value);
  /** Back to the defaults. */
  void reset(const ToolDef &def);
  uint64_t version(const ToolDef &def) const;

  std::string serialize() const;
  /** Merges `text` in. A malformed line is skipped, and the first is returned as
   * InvalidArgument naming its line. */
  Status load(std::string_view text);

  /** Adds the `toolSettings` collection (keyed by tool id) to `root`. */
  void addCollectionTo(ApiStruct &root, std::string_view memberName = kMember);
  /** The struct of a tool's settings, registered on first use. */
  StructId itemStruct(const ToolDef &def);
  static StructId baseStruct();

private:
  struct Entry : PropertyBag {
    const ToolDef *tool = nullptr;
    uint64_t version = 1;
  };
  Entry &entry(const ToolDef &def);
  const Entry *findEntry(const ToolDef &def) const;

  Runtime &rt_;
  litestl::util::Vector<Entry *> entries_;
  litestl::util::Map<uint64_t, Entry *> byKey_;
};

} // namespace coredcc
