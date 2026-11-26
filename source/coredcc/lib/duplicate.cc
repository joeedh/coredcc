#include "coredcc/lib/duplicate.h"

#include "coredcc/lib/io/fill.h"
#include "coredcc/lib/payload_access.h"

#include <string>
#include <utility>

namespace coredcc {

using litestl::util::string;
using litestl::util::Vector;

namespace {

Status naming(const Datablock &b, const Status &s)
{
  return Status::error(
      s.code(),
      ("duplicating '" + std::string(b.name().c_str()) + "': " + s.message()).c_str());
}

class Duplicator {
public:
  explicit Duplicator(Library &lib)
      : lib_(lib), rt_(lib.runtime()), types_(rt_.ext<BlockTypeRegistry>()),
        attachments_(rt_.ext<AttachmentTypeRegistry>()),
        filler_(lib, report_, UINT64_MAX, 0)
  {
  }

  Result<IdMap> run(std::span<const BlockHandle> blocks)
  {
    Status s = copyAll(blocks);
    if (s.ok()) {
      return std::move(ids_);
    }
    filler_.destroy(made_);
    rt_.takeStickyError().ignore(); /* from destroying; `s` came first */
    return s;
  }

private:
  struct Copy {
    BlockHandle from;
    BlockHandle to;
    bool unread = false;
  };

  Status copyAll(std::span<const BlockHandle> blocks)
  {
    for (BlockHandle h : blocks) {
      const Datablock *b = lib_.get(h);
      if (!b) {
        return Status::error(StatusCode::NotFound, "duplicateBlocks: no such block");
      }
      if (ids_.find(b->id()).isNull()) {
        if (Status s = copyBlock(h, *b); !s.ok()) {
          return naming(*b, s);
        }
      }
    }
    CDCC_TRY(remapAll());
    {
      detail::LibraryFiller::Scope scope(lib_);
      for (const Copy &c : copies_) {
        CDCC_TRY(finish(c));
      }
    }
    return remapAll(); /* what was read, and the attachments */
  }

  Result<BlockId> freshId(ClientId client)
  {
    for (int attempt = 0; attempt < 4; attempt++) {
      const BlockId id{client, rt_.newLocalId()};
      if (!id.isNull() && !lib_.find(id)) {
        return id;
      }
    }
    return Status::error(StatusCode::Internal,
                         "the GUID source keeps returning ids in use");
  }

  static Status sticky(Runtime &rt, Status s)
  {
    return rt.hasStickyError() ? rt.takeStickyError() : s;
  }

  /* Through the provider's `duplicate`. The record comes first, so the data always has
   * one. */
  Result<BlockHandle>
  duplicate(const Datablock &b, const BlockTypeRegistry::Entry &type, BlockId id)
  {
    PlaceholderInfo info;
    info.name = b.name();
    info.typeName = type.info.name;
    Result<BlockHandle> made = lib_.addPlaceholder(id, std::move(info));
    if (!made.ok()) {
      return made;
    }
    made_.append(*made);
    BlockCopy out;
    detail::LibraryFiller::Scope scope(lib_);
    BlockProvider p = type.provider.get();
    Status s = sticky(rt_, p.duplicate(b, out));
    void *data = detail::PayloadAccess::data(out);
    if (s.ok() && !data) {
      s = Status::error(StatusCode::Internal,
                        "duplicate succeeded without calling setData");
    }
    CDCC_TRY(filler_.setData(*made, data, std::move(s)));
    return made;
  }

  Status copyBlock(BlockHandle h, const Datablock &b)
  {
    Result<BlockId> id = freshId(b.id().client);
    if (!id.ok()) {
      return id.status();
    }
    Copy copy{h, BlockHandle(), false};
    Result<BlockHandle> made = BlockHandle();
    const BlockTypeRegistry::Entry *type = types_.find(b.type());
    if (b.state() == BlockState::Live && type->provider->hasDuplicate()) {
      made = duplicate(b, *type, *id);
    } else {
      PlaceholderInfo info;
      info.name = b.name();
      info.sourceName = b.sourceStored() ? b.sourceName() : string();
      if (b.state() == BlockState::Live) {
        Result<BlockPayload> payload = writePayload(lib_, h);
        if (!payload.ok()) {
          return payload.status();
        }
        info.typeName = type->info.name;
        info.payload = std::move(payload).value();
        copy.unread = true;
      } else if (b.state() == BlockState::Placeholder &&
                 b.placeholderReason() == PlaceholderReason::MissingType)
      {
        info.typeName = b.placeholderType();
        info.payload = b.placeholderData();
      } else {
        return Status::error(
            StatusCode::FailedPrecondition,
            (std::string("it is ") + blockStateName(b.state()) + ", without its data")
                .c_str());
      }
      made = lib_.addPlaceholder(*id, std::move(info));
      if (made.ok()) {
        made_.append(*made);
      }
    }
    if (!made.ok()) {
      return made.status();
    }
    copy.to = *made;
    copies_.append(copy);
    ids_.add(b.id(), *id);
    lib_.setFlag(copy.to, BlockFlags::FakeUser, b.hasFlag(BlockFlags::FakeUser));
    filler_.setExtension(copy.to, b.fileExtension());
    lib_.forEachSlot(h, [&](TypeId ns, int64_t key, const RefCell &cell) {
      RefCell own(cell.kind);
      own.target = cell.target;
      filler_.addSlot(copy.to, ns, key, own);
    });
    lib_.forEachOpaqueAttachment(
        h, [&](const OpaqueAttachment &o) { filler_.addOpaque(copy.to, o); });
    return Status();
  }

  /* Reads the payload and copies the attachments. */
  Status finish(const Copy &c)
  {
    if (c.unread && !filler_.fill(c.to)) {
      return naming(*lib_.get(c.from), report_.issues.first[0].why);
    }
    Status status;
    lib_.forEachAttachment(c.from, [&](TypeId type, void *data) {
      const AttachmentTypeRegistry::Entry *e = attachments_.find(type);
      if (!status.ok() || e->info.onDuplicate != DuplicatePolicy::Copy) {
        return;
      }
      AttachmentProvider p = e->provider.get();
      void *copy = p.copy(*lib_.get(c.from), data, *lib_.get(c.to));
      if (copy) {
        filler_.addAttachment(c.to, type, copy);
      }
      status = sticky(rt_,
                      copy ? Status()
                           : Status::error(StatusCode::Internal,
                                           "an attachment's copy gave no data"));
    });
    return status.ok() ? status : naming(*lib_.get(c.from), status);
  }

  Status remapAll()
  {
    for (BlockHandle h : made_) {
      Status s = lib_.forEachRef(h, [this](RefCell &cell) {
        const BlockId to =
            cell.isNull() ? BlockId() : ids_.find(clients().canonical(cell.target));
        if (!to.isNull()) {
          cell.target = to;
        }
        cell.cache = BlockHandle();
      });
      if (s = sticky(rt_, std::move(s)); !s.ok()) {
        return naming(*lib_.get(h), s);
      }
    }
    lib_.noteRefsChanged();
    return Status();
  }

  const ClientRegistry &clients() const
  {
    return rt_.ext<ClientRegistry>();
  }

  Library &lib_;
  Runtime &rt_;
  const BlockTypeRegistry &types_;
  const AttachmentTypeRegistry &attachments_;
  LoadReport report_;
  detail::LibraryFiller filler_;
  IdMap ids_;
  Vector<BlockHandle> made_;
  Vector<Copy> copies_;
};

} // namespace

Result<IdMap> duplicateBlocks(Library &lib, std::span<const BlockHandle> blocks)
{
  CDCC_TRY(detail::LibraryFiller::ready(lib, "duplicateBlocks"));
  return Duplicator(lib).run(blocks);
}

} // namespace coredcc
