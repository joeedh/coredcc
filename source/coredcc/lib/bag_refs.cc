#include "coredcc/lib/bag_refs.h"

#include "coredcc/base/text.h"

#include <utility>

namespace coredcc {

namespace {

/* The value's pointer, if it is into a block. */
const StableRef *blockPointer(const PropValue &v)
{
  const StableRef *ref = v.tryPointer();
  return ref && !ref->isNull() && ref->owner.kind == OwnerKey::Kind::Block ? ref
                                                                           : nullptr;
}

} // namespace

const RefCell *BagRefs::find(std::string_view name) const
{
  for (const Entry &e : entries_) {
    if (name == e.name.c_str()) {
      return &e.cell;
    }
  }
  return nullptr;
}

RefCell &BagRefs::cell(std::string_view name)
{
  if (const RefCell *c = find(name)) {
    return const_cast<RefCell &>(*c);
  }
  entries_.append(Entry{lsString(name), RefCell(kind_)});
  return entries_.last().cell;
}

Status BagRefs::setAt(PropertyBag &bag, int index, const PropValue &value)
{
  CDCC_TRY(bag.setAt(index, value));
  const char *name = bag.def(index).apiName().c_str();
  const StableRef *ref = blockPointer(bag.value(index));
  if (ref || find(name)) {
    RefCell &c = cell(name);
    c.target = ref ? ref->owner.block : BlockId();
    c.cache = BlockHandle();
  }
  return Status();
}

Status BagRefs::set(PropertyBag &bag, std::string_view name, const PropValue &value)
{
  const int index = bag.indexOf(name);
  return index < 0 ? bag.set(name, value) /* says so */ : setAt(bag, index, value);
}

Status BagRefs::reset(PropertyBag &bag, std::string_view name)
{
  CDCC_TRY(bag.reset(name));
  if (find(name)) {
    RefCell &c = cell(name);
    c.target = BlockId();
    c.cache = BlockHandle();
  }
  return Status();
}

PropValue BagRefs::value(const PropertyBag &bag, int index) const
{
  const PropValue &v = bag.value(index);
  const RefCell *c = find(bag.def(index).apiName().c_str());
  if (!c) {
    return v;
  }
  const StableRef *ref = blockPointer(v);
  if (c->isNull()) {
    return ref ? PropValue::nullPointer() : v;
  }
  StableRef out;
  if (ref) {
    out = *ref;
  } else if (const TargetMeta *meta = bag.def(index).target()) {
    out.type = meta->target;
  }
  out.owner = OwnerKey::ofBlock(c->target);
  return PropValue::ofPointer(std::move(out));
}

bool BagRefs::isSet(const PropertyBag &bag, int index) const
{
  const RefCell *c = find(bag.def(index).apiName().c_str());
  return bag.wasSet(index) || (c && !c->isNull());
}

void BagRefs::visit(RefVisitor &visitor)
{
  for (Entry &e : entries_) {
    visitor.visit(e.cell);
  }
}

} // namespace coredcc
