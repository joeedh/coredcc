#pragma once

#include "coredcc/base/block_id.h"
#include "coredcc/base/id.h"
#include "coredcc/base/status.h"

#include "litestl/util/alloc.h"
#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/string.h"
#include "litestl/util/vector.h"

#include <string_view>

namespace coredcc {

class IfaceRegistry;

/**
 * The root of one coredcc instance. Every registry belongs to a Runtime; there is no
 * process-global one. Several Runtimes may coexist in a process (e.g. two hosts).
 *
 * It holds the name registry (hashed ids back to names, rejecting collisions) and the
 * interface registry. Later modules' registries (block types, the API, commands) are
 * extensions: see ext().
 */
class Runtime {
 public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime &) = delete;
  Runtime &operator=(const Runtime &) = delete;

  /** Interfaces and their implementations (coredcc/base/iface/registry.h). */
  IfaceRegistry &ifaces()
  {
    return *ifaces_;
  }
  const IfaceRegistry &ifaces() const
  {
    return *ifaces_;
  }

  /** Re-checks every registry's invariants. Run at startup in debug builds and in
   * every test binary. */
  Status validateAll() const;

  /**
   * The extension of type T, created on first use. base can't name the registries of
   * the modules above it, so they live here, type-erased. T provides:
   * - `static constexpr const char *kExtName`, a stable unique name;
   * - `explicit T(Runtime *rt)`;
   * - optionally `Status validate() const`, which validateAll() calls.
   *
   * Extensions are destroyed in reverse order of creation, before the interface
   * registry, so an extension may use any extension it created in its constructor.
   */
  template <class T> T &ext()
  {
    if (T *found = findExt<T>()) {
      return *found;
    }
    for (const char *pending : constructing_) {
      CDCC_CHECK(pending != T::kExtName, "an extension's constructor asked for itself");
    }
    constructing_.append(T::kExtName);
    T *obj = litestl::alloc::New<T>(T::kExtName, this);
    constructing_.pop_back();
    ExtSlot slot;
    slot.key = hashId(IdDomain::Type, T::kExtName);
    slot.tag = extTag<T>();
    slot.obj = obj;
    slot.destroy = &destroyExt<T>;
    if constexpr (requires(const T &t) { t.validate(); }) {
      slot.validate = [](const void *p) { return static_cast<const T *>(p)->validate(); };
    }
    exts_.append(slot);
    return *obj;
  }

  /** The extension of type T if it has been created, else null. */
  template <class T> T *findExt() const
  {
    const uint64_t key = hashId(IdDomain::Type, T::kExtName);
    for (const ExtSlot &slot : exts_) {
      if (slot.key == key) {
        /* Two extension types with one name would alias each other's memory. */
        CDCC_CHECK(slot.tag == extTag<T>(), "two extension types share a kExtName");
        return static_cast<T *>(slot.obj);
      }
    }
    return nullptr;
  }

  /**
   * The source of new LocalIds for the built-in coredcc client: a host GUID function,
   * or by default the OS's randomness (getentropy, or RtlGenRandom on Windows). Never a
   * seeded PRNG: ids must not repeat across sessions. An empty function restores the
   * default.
   */
  using GuidSource = litestl::util::function<LocalId()>;
  void setGuidSource(GuidSource source);
  /** A new random LocalId; never null. */
  LocalId newLocalId();

  /**
   * The next version stamp: monotonic, never 0, and never stored in a file or a
   * snapshot. Undo, redo and restore stamp fresh versions instead of putting old ones
   * back, so equal versions mean the same state.
   */
  uint64_t nextVersion()
  {
    return ++version_;
  }
  /** nextVersion(), recorded as an edit: something a graph evaluation may depend on
   * changed. */
  uint64_t touch()
  {
    lastEdit_ = nextVersion();
    return lastEdit_;
  }
  /** The version of the last touch(); 0 before the first. */
  uint64_t lastEdit() const
  {
    return lastEdit_;
  }

  /** Whether a graph is evaluating (an EvaluationScope is open). Until it ends, nothing
   * may change the document: setValue and every CommandExecutor call are Busy. */
  bool evaluating() const
  {
    return evaluating_ > 0;
  }
  /** Busy, naming `what`, while evaluating(); else ok. */
  Status busyIfEvaluating(const char *what) const;

  /**
   * The sticky error: set by calls that can't return a Status (a failed foreign
   * interface call), checked by the caller afterwards (the command executor checks it
   * after each command). The first error wins until it is taken.
   */
  void setStickyError(Status status);
  bool hasStickyError() const
  {
    return !stickyError_.ok();
  }
  /** Returns the sticky error and clears it. */
  Status takeStickyError();

  /**
   * Register `name` in `domain`, returning its id. Registering the same name twice is
   * fine and returns the same id; a different name that hashes to the same id is an
   * AlreadyExists error naming both.
   */
  Result<uint64_t> registerName(IdDomain domain, std::string_view name);

  template <IdDomain D> Result<Id<D>> registerId(std::string_view name)
  {
    Result<uint64_t> r = registerName(D, name);
    if (!r.ok()) {
      return r.status();
    }
    return Id<D>{*r};
  }

  /** Test hook: register `name` under a forced id, to exercise collision handling. */
  Result<uint64_t> debugRegisterNameAs(IdDomain domain, uint64_t id, std::string_view name);

  /** The registered name for an id, or nullptr. */
  const char *nameOf(IdDomain domain, uint64_t id) const;
  template <IdDomain D> const char *nameOf(Id<D> id) const
  {
    return nameOf(D, id.value);
  }

 private:
  friend class EvaluationScope;
  template <class T> static void destroyExt(void *p)
  {
    litestl::alloc::Delete(static_cast<T *>(p));
  }
  /* One address per extension type. Writable, so the linker never folds two of them
   * (as it may fold identical functions). */
  template <class T> static const void *extTag()
  {
    static char tag;
    return &tag;
  }
  struct ExtSlot {
    uint64_t key = 0;
    const void *tag = nullptr;
    void *obj = nullptr;
    void (*destroy)(void *) = nullptr;
    Status (*validate)(const void *) = nullptr;
  };

  litestl::util::Map<uint64_t, litestl::util::string> names_[kIdDomainCount];
  litestl::util::Vector<ExtSlot> exts_;
  litestl::util::Vector<const char *> constructing_;
  IfaceRegistry *ifaces_;
  Status stickyError_;
  GuidSource guidSource_;
  uint64_t version_ = 0;
  uint64_t lastEdit_ = 0;
  int evaluating_ = 0;
};

/** Makes `rt` evaluating() while it lives. Opened by the graph evaluator, around a whole
 * evaluation; scopes nest. The Runtime must outlive it. */
class EvaluationScope {
 public:
  explicit EvaluationScope(Runtime &rt) : rt_(rt)
  {
    rt_.evaluating_++;
  }
  ~EvaluationScope()
  {
    rt_.evaluating_--;
  }
  EvaluationScope(const EvaluationScope &) = delete;
  EvaluationScope &operator=(const EvaluationScope &) = delete;

 private:
  Runtime &rt_;
};

} // namespace coredcc
