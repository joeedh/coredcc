#include "coredcc/api/attachments.h"

#include "coredcc/lib/attachment.h"
#include "coredcc/lib/library.h"

#include <cstring>

namespace coredcc {

namespace {

struct Item {
  const AttachmentTypeRegistry::Entry *type = nullptr;
  void *data = nullptr;
};

/* Calls fn(Item) for each attachment with a data struct, until it returns true. */
template <class Fn> void eachAttachment(const PointerRef &self, Context &ctx, Fn &&fn)
{
  Library *lib = ctx.library();
  if (!lib || self.owner.kind != OwnerKey::Kind::Block) {
    return;
  }
  const AttachmentTypeRegistry &types = ctx.runtime().ext<AttachmentTypeRegistry>();
  bool done = false;
  lib->forEachAttachment(lib->find(self.owner.block), [&](TypeId type, void *data) {
    const AttachmentTypeRegistry::Entry *e = types.find(type);
    if (!done && e && e->info.dataStruct) {
      done = fn(Item{e, data});
    }
  });
}

PointerRef pointerTo(const Item &item)
{
  return item.type ? PointerRef{item.data, item.type->info.dataStruct, OwnerKey()} : PointerRef();
}

} // namespace

void addAttachments(ApiStruct &st)
{
  CollectionAccessor a;
  a.length = [](const PointerRef &self, Context &ctx) {
    int64_t n = 0;
    eachAttachment(self, ctx, [&](const Item &) {
      n++;
      return false;
    });
    return n;
  };
  a.at = [](const PointerRef &self, Context &ctx, int64_t index) {
    Item found;
    eachAttachment(self, ctx, [&](const Item &item) {
      found = item;
      return index-- == 0;
    });
    return index < 0 ? pointerTo(found) : PointerRef();
  };
  a.lookup = [](const PointerRef &self, Context &ctx, const KeySegment &key) {
    Item found;
    eachAttachment(self, ctx, [&](const Item &item) {
      if (key.kind == KeySegment::Kind::StringKey &&
          strcmp(item.type->info.name.c_str(), key.text.c_str()) == 0)
      {
        found = item;
      }
      return found.type != nullptr;
    });
    return pointerTo(found);
  };
  a.keyOf = [](const PointerRef &self, Context &ctx, const PointerRef &item) {
    KeySegment key = KeySegment::stringKey("");
    eachAttachment(self, ctx, [&](const Item &i) {
      if (i.data == item.data && i.type->info.dataStruct == item.type) {
        key = KeySegment::stringKey(i.type->info.name.c_str());
        return true;
      }
      return false;
    });
    return key;
  };
  st.addCollection(PropertyDef::collectionProp("attachments", StructId::of("coredcc.Attachment")),
                   std::move(a));
}

} // namespace coredcc
