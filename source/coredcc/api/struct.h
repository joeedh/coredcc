#pragma once

/** \file
 * ApiStruct and ApiMember: the data model the datapath exposes, which need not match
 * the C++ layout (architecture §5), and ApiRegistry, where structs and runtime owners
 * are registered.
 */

#include "coredcc/api/context.h"
#include "coredcc/lib/block.h"
#include "coredcc/props/def.h"

#include "litestl/util/function.h"
#include "litestl/util/map.h"
#include "litestl/util/vector.h"

#include <limits>
#include <string_view>
#include <type_traits>

namespace coredcc {

/** A value member's accessor. An empty `set` makes the member read-only. */
struct ValueAccessor {
  litestl::util::function<PropValue(const PointerRef &self, Context &ctx)> get;
  /** Receives a value already coerced to the member's def. */
  litestl::util::function<Status(const PointerRef &self, Context &ctx, const PropValue &v)> set;
  /** Puts a stored value back, into the same member of the same instance. */
  using RestoreFn = litestl::util::function<Status(const PointerRef &self, Context &ctx)>;
  /** What `save` returns. */
  struct Stored {
    RestoreFn restore;
    /** About what it keeps, for the undo history's budget. */
    uint64_t bytes = 0;
  };
  /**
   * Optional, for a member that `set(get())` would change: one that can be unset, where `get`
   * gives a default, or that stores more than `get` gives. Returns what puts back what is stored
   * now, without checks. Undo then uses it; it must not keep pointers into the instance.
   */
  litestl::util::function<Stored(const PointerRef &self, Context &ctx)> save = {};
};

/** A Pointer member: a nested struct, static or resolved per instance. */
struct PointerAccessor {
  /** The target, with its owner set when it lives in another block. A null `data` is a
   * null pointer. The type may be left null: `target` or `refine` supplies it. */
  litestl::util::function<PointerRef(const PointerRef &self, Context &ctx)> get;
  /** Optional: makes the pointer settable (a null target clears it). */
  litestl::util::function<Status(const PointerRef &self, Context &ctx, const PointerRef &target)>
      set;
  /** The target struct when it is static. */
  StructId target;
  /** Otherwise the struct of a given target, from `candidates` (declared so paths
   * through it can be checked statically). Subtypes of either are accepted. */
  litestl::util::function<StructId(const PointerRef &target)> refine;
  litestl::util::Vector<StructId, 1> candidates;
  /**
   * The target's own address, for a settable pointer whose targets live inside the same
   * owner (`activeBrush` into `brushes`). Without it, a Pointer value or a canonical
   * path through the pointer could only say "whatever this points at now", which undo
   * can't use, so reading or walking such a pointer is Unsupported. Pointers into
   * another owner don't need it: they must return that owner's root struct.
   */
  litestl::util::function<StableRef(const PointerRef &self, Context &ctx, const PointerRef &target)>
      address;
  /**
   * Optional, for a settable pointer to a block: the RefCell it is stored in (null for
   * none). Undo then restores a link to a block that isn't Live (a placeholder, a deleted
   * block): such a link reads as null, so setting the read value back would lose it.
   */
  litestl::util::function<RefCell *(const PointerRef &self, Context &ctx)> cell;
};

/** A Collection member. `keyOf` is mandatory: undo stores items by key. */
struct CollectionAccessor {
  litestl::util::function<int64_t(const PointerRef &self, Context &ctx)> length;
  litestl::util::function<PointerRef(const PointerRef &self, Context &ctx, int64_t index)> at;
  /** By a string key or a handle; a null result means no such item. */
  litestl::util::function<PointerRef(const PointerRef &self, Context &ctx, const KeySegment &key)>
      lookup;
  /** A stable key for `item`: a StringKey or a Handle segment. */
  litestl::util::function<KeySegment(const PointerRef &self, Context &ctx, const PointerRef &item)>
      keyOf;
  /** Optional: `list.active`. */
  litestl::util::function<PointerRef(const PointerRef &self, Context &ctx)> active;
  /**
   * Optional: the settable Pointer member, of the struct the collection is in, that holds the
   * active item. Writing `list.active` then writes that member. Watch or read the member itself
   * where there may be no active item: `list.active` is NotFound then.
   */
  litestl::util::string activeMember;
  /** The items' struct (an item's own `type`, when set, wins). */
  StructId itemStruct;
  /** Optional, for static checks of paths: the struct of the item with this key, or why there
   * is none. Without it, an item is an `itemStruct` or inherits from it. */
  litestl::util::function<Result<StructId>(const KeySegment &key)> staticItem;
};

using MemberUpdateFn = litestl::util::function<void(const PointerRef &self, Context &ctx)>;
/** Patches a copy of the member's def for one instance, with the Context (the props
 * hook can't see one). */
using MemberDynamicFn =
    litestl::util::function<void(PropertyDef &def, const PointerRef &self, Context &ctx)>;

enum class MemberKind : uint8_t { Value, Pointer, Collection };

class ApiMember {
 public:
  ApiMember(PropertyDef def) : def_(std::move(def)) {}

  /** Called after every successful set through setValue (step 5). */
  ApiMember &update(MemberUpdateFn fn)
  {
    update_ = std::move(fn);
    return *this;
  }
  ApiMember &dynamic(MemberDynamicFn fn)
  {
    dynamic_ = std::move(fn);
    return *this;
  }

  const PropertyDef &def() const
  {
    return def_;
  }
  const char *name() const
  {
    return def_.apiName().c_str();
  }
  MemberKind kind() const
  {
    return def_.kind() == PropKind::Pointer    ? MemberKind::Pointer :
           def_.kind() == PropKind::Collection ? MemberKind::Collection :
                                                 MemberKind::Value;
  }
  /** Read-only by flag, or for lack of a setter. */
  bool readOnly() const;

  const ValueAccessor &value() const
  {
    return value_;
  }
  const PointerAccessor &pointer() const
  {
    return pointer_;
  }
  const CollectionAccessor &collection() const
  {
    return collection_;
  }
  const MemberUpdateFn &updateFn() const
  {
    return update_;
  }
  const MemberDynamicFn &dynamicFn() const
  {
    return dynamic_;
  }

 private:
  friend class ApiStruct;
  PropertyDef def_;
  ValueAccessor value_;
  PointerAccessor pointer_;
  CollectionAccessor collection_;
  MemberUpdateFn update_;
  MemberDynamicFn dynamic_;
};

namespace detail {
template <class M> constexpr int arrayLength()
{
  if constexpr (std::is_array_v<M>) {
    return int(std::extent_v<M>);
  }
  else {
    return 0;
  }
}
} // namespace detail

/**
 * A struct of the data model: an id, UI text, an optional parent whose members it
 * inherits, and ordered members. Build it, then register it with ApiRegistry, which
 * makes it immutable. A returned ApiMember& is valid until the next add.
 */
class ApiStruct {
 public:
  explicit ApiStruct(std::string_view name);

  ApiStruct &uiName(std::string_view s);
  ApiStruct &description(std::string_view s);
  ApiStruct &parent(StructId parent);

  ApiMember &add(PropertyDef def, ValueAccessor accessor);
  /** A value member stored in a field of T: bool, an integer, an enum, float, double,
   * litestl string, or a float/double array of the def's component count. */
  template <class T, class M> ApiMember &addField(PropertyDef def, M T::*field);
  ApiMember &addPointer(PropertyDef def, PointerAccessor accessor);
  ApiMember &addCollection(PropertyDef def, CollectionAccessor accessor);

  StructId id() const
  {
    return id_;
  }
  const litestl::util::string &name() const
  {
    return name_;
  }
  const litestl::util::string &uiNameText() const
  {
    return uiName_;
  }
  const litestl::util::string &descriptionText() const
  {
    return description_;
  }
  StructId parentId() const
  {
    return parent_;
  }
  int memberCount() const
  {
    return int(members_.size());
  }
  const ApiMember &member(int i) const
  {
    return members_[i];
  }
  /** This struct's own member (not its parent's), or null. */
  const ApiMember *findOwn(std::string_view name) const;

 private:
  StructId id_;
  litestl::util::string name_;
  litestl::util::string uiName_;
  litestl::util::string description_;
  StructId parent_;
  litestl::util::Vector<ApiMember, 1> members_;
};

/** Runtime owners' names are Type names that start with this. */
inline constexpr const char *kOwnerNamePrefix = "owner:";

/**
 * Registered structs and runtime owners. A Runtime extension.
 *
 * A **runtime owner** is non-block data addressable by StableRef: tool settings,
 * preferences, the LastUsedStore, the context root. It registers under a stable name;
 * its OwnerKey holds the name's id, and collisions are rejected.
 */
class ApiRegistry {
 public:
  static constexpr const char *kExtName = "coredcc.ApiRegistry";

  explicit ApiRegistry(Runtime *rt) : rt_(*rt) {}
  ~ApiRegistry();
  ApiRegistry(const ApiRegistry &) = delete;
  ApiRegistry &operator=(const ApiRegistry &) = delete;

  /**
   * Registers a struct after checking it: valid, uniquely named member defs; value
   * members with a getter; pointers with a getter and a static target or a refine with
   * candidates; collections with length, at, lookup and keyOf.
   */
  Result<const ApiStruct *> add(ApiStruct st);
  const ApiStruct *find(StructId id) const;
  /** Registered structs, in registration order (for listings and static checks). */
  int structCount() const
  {
    return int(structs_.size());
  }
  const ApiStruct &structAt(int i) const
  {
    return *structs_[i];
  }
  /** `type` is `base` or has it among its parents. */
  bool isA(StructId type, StructId base) const;
  /** A member of `type` or of its parents. */
  const ApiMember *findMember(StructId type, std::string_view name) const;
  /** A NotFound Status for `name` in `type`, with suggestions from every member. */
  Status unknownMember(StructId type, std::string_view name) const;

  /** Registers a runtime owner; `data` returns its current instance. */
  Result<OwnerKey> addRuntimeOwner(std::string_view name,
                                   StructId type,
                                   litestl::util::function<void *()> data);
  /** The owner's instance, or a null PointerRef. */
  PointerRef runtimeOwner(const OwnerKey &owner) const;
  /** The PointerRef for a runtime owner by name (for a Context root). */
  PointerRef runtimeOwner(std::string_view name) const;

  Status validate() const;

 private:
  struct Owner {
    StructId type;
    litestl::util::function<void *()> data;
  };
  Runtime &rt_;
  litestl::util::Vector<ApiStruct *> structs_;
  litestl::util::Map<uint64_t, ApiStruct *> byId_;
  litestl::util::Map<uint64_t, Owner> owners_;
};

/* ---- field accessors ---- */

namespace detail {

template <class M> PropValue fieldToValue(const M &field, PropKind kind)
{
  if constexpr (std::is_array_v<M>) {
    double c[16] = {};
    for (int i = 0; i < arrayLength<M>(); i++) {
      c[i] = double(field[i]);
    }
    return PropValue::ofComponents(valueTypeFor(kind), c);
  }
  else if constexpr (std::is_same_v<M, litestl::util::string>) {
    return PropValue::ofString(field.c_str());
  }
  else if constexpr (std::is_same_v<M, bool>) {
    return PropValue::ofBool(field);
  }
  else if constexpr (std::is_floating_point_v<M>) {
    return PropValue::ofFloat(double(field));
  }
  else if constexpr (std::is_integral_v<M> || std::is_enum_v<M>) {
    using U = std::conditional_t<std::is_enum_v<M>, std::underlying_type<M>,
                                 std::type_identity<M>>;
    const auto raw = static_cast<typename U::type>(field);
    if (kind == PropKind::Enum) {
      return PropValue::ofEnum(int64_t(raw));
    }
    if (kind == PropKind::Flags) {
      return PropValue::ofFlags(uint64_t(raw));
    }
    return PropValue::ofInt(int64_t(raw));
  }
  else {
    static_assert(sizeof(M) == 0, "unsupported field type for addField");
  }
}

template <class M> Status valueToField(const PropValue &v, M &field)
{
  if constexpr (std::is_array_v<M>) {
    for (int i = 0; i < arrayLength<M>(); i++) {
      field[i] = std::remove_extent_t<M>(v.component(i));
    }
  }
  else if constexpr (std::is_same_v<M, litestl::util::string>) {
    field = v.asString();
  }
  else if constexpr (std::is_same_v<M, bool>) {
    field = v.asBool();
  }
  else if constexpr (std::is_floating_point_v<M>) {
    field = M(v.asFloat());
  }
  else if constexpr (std::is_integral_v<M> || std::is_enum_v<M>) {
    using U = typename std::conditional_t<std::is_enum_v<M>, std::underlying_type<M>,
                                          std::type_identity<M>>::type;
    const int64_t raw = v.type() == PropValue::Type::Enum  ? v.asEnum() :
                        v.type() == PropValue::Type::Flags ? int64_t(v.asFlags()) :
                                                             v.asInt();
    if (raw < int64_t((std::numeric_limits<U>::min)()) ||
        (raw > 0 && uint64_t(raw) > uint64_t((std::numeric_limits<U>::max)())))
    {
      return Status::error(StatusCode::OutOfRange, "value out of the field's range");
    }
    field = static_cast<M>(static_cast<U>(raw));
  }
  return Status();
}

} // namespace detail

template <class T, class M> ApiMember &ApiStruct::addField(PropertyDef def, M T::*field)
{
  if constexpr (std::is_array_v<M>) {
    CDCC_CHECK(detail::arrayLength<M>() == componentCount(def.kind()),
               "addField: the array length is not the def's component count");
  }
  const PropKind kind = def.kind();
  ValueAccessor a;
  a.get = [field, kind](const PointerRef &self, Context &) {
    return detail::fieldToValue(static_cast<const T *>(self.data)->*field, kind);
  };
  a.set = [field](const PointerRef &self, Context &, const PropValue &v) {
    return detail::valueToField(v, static_cast<T *>(self.data)->*field);
  };
  return add(std::move(def), std::move(a));
}

} // namespace coredcc
