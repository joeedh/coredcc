#pragma once

/** \file
 * LastUsedStore: each command's last-used input values (architecture §6, "Registry,
 * defaults and invocation").
 */

#include "coredcc/api/struct.h"
#include "coredcc/command/command.h"

#include <string>
#include <string_view>

namespace coredcc {

/**
 * The input values a command last ran with, per command. A Runtime extension.
 *
 * - The executor remembers a top-level command's inputs after it succeeds: the ones
 *   that were set, Pointer and Collection inputs excepted.
 * - `apply` puts them in a fresh input bag (`invoke` does this), skipping any the def
 *   no longer accepts.
 * - The datapath sees them as `commandDefaults["mesh.extrude"].dist` once
 *   `addCollectionTo` has added that collection to the host's root struct. Reading an
 *   input that was never used gives its default. Watchers see it change.
 * - `serialize` writes one invocation per line; `load` reads it back, skipping
 *   commands and inputs that no longer exist, so old files keep loading.
 */
class LastUsedStore {
 public:
  static constexpr const char *kExtName = "coredcc.LastUsedStore";
  /** The collection's name, which layouts expect. */
  static constexpr const char *kMember = "commandDefaults";

  explicit LastUsedStore(Runtime *rt) : rt_(*rt) {}
  ~LastUsedStore();
  LastUsedStore(const LastUsedStore &) = delete;
  LastUsedStore &operator=(const LastUsedStore &) = delete;

  void remember(const CommandDef &def, const PropertyBag &inputs);
  void apply(const CommandDef &def, PropertyBag &bag) const;
  /** The remembered value, or null. */
  const PropValue *get(const CommandDef &def, std::string_view input) const;
  /** Coerces `value` to the input's def and remembers it. */
  Status set(const CommandDef &def, std::string_view input, const PropValue &value);
  void forget(const CommandDef &def);
  void clear();

  std::string serialize() const;
  /** Merges `text` in. A malformed line is skipped and the rest still load; the first
   * one is returned as InvalidArgument naming its line. */
  Status load(std::string_view text);

  /** Adds the `commandDefaults` collection (keyed by command id) to `root`. */
  void addCollectionTo(ApiStruct &root, std::string_view memberName = kMember);

  Status validate() const;

 private:
  /* The command's inputs, of which those that are remembered are set. The bag is what a
   * `commandDefaults` item points to: dynamic inputs are resolved with it. */
  struct Entry : PropertyBag {
    const CommandDef *command = nullptr;
  };
  void changed();
  void destroyEntries();
  Entry &entry(const CommandDef &def);
  const Entry *findEntry(const CommandDef &def) const;
  /** The per-command struct of commandDefaults items, registered on first use. */
  StructId itemStruct(const CommandDef &def);

  Runtime &rt_;
  litestl::util::Vector<Entry *> entries_;
  litestl::util::Map<uint64_t, Entry *> byId_;
};

} // namespace coredcc
