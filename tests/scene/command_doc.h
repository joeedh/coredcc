#pragma once

/* A scene document with an executor and a history, and the helpers of the command tests. */

#include "scene_doc.h"

#include "coredcc/command/block_commands.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/set_property.h"
#include "coredcc/scene/commands.h"

#include <initializer_list>
#include <utility>
#include <vector>

namespace scene_test {

using Inputs = std::initializer_list<std::pair<const char *, PropValue>>;

struct CommandDoc : Doc {
  std::unique_ptr<LinearHistory> history = std::make_unique<LinearHistory>();
  std::unique_ptr<CommandExecutor> ex;

  CommandDoc()
  {
    must(registerSceneCommands(rt));
    must(registerSceneCommands(rt)); /* idempotent */
    must(rt.ext<CommandRegistry>().validate());
    ex = std::make_unique<CommandExecutor>(&rt, history.get());
    ctx->setExecutor(ex.get());
  }
  ~CommandDoc()
  {
    ctx->setExecutor(nullptr);
    ex.reset();
    history.reset();
  }

  PropValue ptr(BlockHandle h)
  {
    return h ? detail::pointerTo(*lib, h) : PropValue::nullPointer();
  }
  Result<PropertyBag> run(const char *command, Inputs inputs = {})
  {
    const CommandDef *def = rt.ext<CommandRegistry>().find(command);
    PropertyBag bag = def->newInputs();
    for (const auto &[name, value] : inputs) {
      must(bag.set(name, value));
    }
    return ex->run(*ctx, *def, std::move(bag));
  }
  /** Runs one that must work. */
  PropertyBag ok(const char *command, Inputs inputs = {})
  {
    Result<PropertyBag> r = run(command, inputs);
    if (!r.ok()) {
      fprintf(stderr, "%s: %s\n", command, r.status().message());
    }
    test_assert(r.ok());
    return r.ok() ? std::move(r).value() : rt.ext<CommandRegistry>().find(command)->newOutputs();
  }
  Status fails(const char *command, Inputs inputs = {})
  {
    const int before = history->size();
    const std::string was = describe();
    Result<PropertyBag> r = run(command, inputs);
    test_assert(!r.ok() && history->size() == before && !rt.hasStickyError());
    test_assert(describe() == was); /* nothing is half-done */
    return r.status();
  }
  BlockHandle blockOut(const PropertyBag &out, const char *name)
  {
    const StableRef *r = out.get(name)->tryPointer();
    return r ? lib->find(r->owner.block) : BlockHandle();
  }
  void select(std::initializer_list<BlockHandle> handles)
  {
    Vector<BlockId> ids;
    for (BlockHandle h : handles) {
      ids.append(idOf(h));
    }
    sc().setSelection(*lib, ids);
  }
  TransformState stateOf(BlockHandle h)
  {
    Result<TransformState> s = computeState(*lib, h, sc().time());
    test_assert(s.ok());
    return s.ok() ? *s : TransformState();
  }

  /** Everything about the document, as text: what undo must bring back. */
  std::string describe()
  {
    std::string out;
    const auto id = [](const BlockId &b) {
      return std::to_string(b.local.hi) + ":" + std::to_string(b.local.lo);
    };
    lib->forEach([&](BlockHandle h, const Datablock &b) {
      out += "block " + std::to_string(h.index) + " " + b.name().c_str() + " " + id(b.id()) + " " +
             std::to_string(int(b.state())) + "\n";
      const bool own = b.type() == TypeId::of(kObjectType);
      const bool foreign = b.type() == TypeId::of("test.Host");
      if (own || foreign) {
        const ChannelBlock &c = own ? obj(h).channels : host(h).channels;
        const TypeId type = own ? obj(h).transformTypeId() : host(h).info.transformType;
        const BlockId parent = own ? obj(h).parent.target() : host(h).info.parent;
        out += " type " + std::to_string(type.value) + " parent " + id(parent) + ":";
        for (int i = 0; i < c.size(); i++) {
          uint64_t bits;
          const double v = c.get(i);
          memcpy(&bits, &v, 8);
          out += " " + std::to_string(bits);
        }
        out += "\n";
      }
      if (b.type() == TypeId::of(kSceneType)) {
        Scene &s = *static_cast<Scene *>(b.clientData());
        out += " members";
        for (const RefCell &c : s.cells()) {
          out += " " + id(c.target);
        }
        out += " selected";
        for (const RefCell &c : s.selection()) {
          out += " " + id(c.target);
        }
        out += " active " + id(s.activeCell().target) + "\n";
      }
    });
    return out;
  }
  void undo()
  {
    must(ex->undo(*ctx));
  }
  void redo()
  {
    must(ex->redo(*ctx));
  }
  /** Runs a command, undoes it, redoes it, undoes and redoes it again, comparing all of it. */
  PropertyBag cycle(const char *command, Inputs inputs = {})
  {
    const std::string before = liveOnly(describe());
    PropertyBag out = ok(command, inputs);
    const std::string after = liveOnly(describe());
    for (int i = 0; i < 2; i++) {
      undo();
      test_assert(liveOnly(describe()) == before);
      redo();
      test_assert(liveOnly(describe()) == after);
    }
    return out;
  }
  /* Blocks a command created stay in the history when it is undone. */
  static std::string liveOnly(const std::string &s)
  {
    std::string out;
    bool live = true;
    size_t at = 0;
    while (at < s.size()) {
      const size_t end = s.find('\n', at) + 1;
      const std::string line = s.substr(at, end - at);
      if (line.compare(0, 5, "block") == 0) {
        live = line[line.size() - 2] == '0' + int(BlockState::Live);
      }
      out += live ? line : "";
      at = end;
    }
    return out;
  }
};

} // namespace scene_test
