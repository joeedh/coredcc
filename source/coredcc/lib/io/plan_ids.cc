#include "coredcc/lib/io/parse.h"

#include "coredcc/lib/library.h"

#include <algorithm>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace coredcc::detail::libio {

using litestl::util::Vector;

namespace {

/* Ids are name hashes, so a file can make two names share one. Such a file is refused: its
 * blocks would be taken for another type's. */
Status checkNames(const ParsedFile &file, const Runtime &rt, const NameTable &known)
{
  struct Use {
    IdDomain domain;
    uint64_t id;
    std::string_view text;
  };
  Vector<Use> uses;
  for (const FileName &n : file.names) {
    if (n.isRaw) {
      continue;
    }
    if (n.asClient) {
      uses.append(Use{IdDomain::Client, n.client.value, n.text});
    }
    if (n.asType) {
      uses.append(Use{IdDomain::Type, n.type.value, n.text});
    }
  }
  const auto clash = [](std::string_view a, std::string_view b) {
    return Status::error(StatusCode::InvalidArgument,
                         ("the library section: the name '" + std::string(a) +
                          "' has the id of '" + std::string(b) + "'")
                             .c_str());
  };
  std::sort(first(uses), last(uses), [](const Use &a, const Use &b) {
    return std::tie(a.domain, a.id, a.text) < std::tie(b.domain, b.id, b.text);
  });
  for (int i = 0; i < int(uses.size()); i++) {
    const Use &u = uses[i];
    if (i > 0 && uses[i - 1].domain == u.domain && uses[i - 1].id == u.id &&
        uses[i - 1].text != u.text)
    {
      return clash(u.text, uses[i - 1].text);
    }
    for (const char *other : {rt.nameOf(u.domain, u.id), known.nameOf(u.domain, u.id)}) {
      if (other && u.text != other) {
        return clash(u.text, other);
      }
    }
  }
  return Status();
}

void addNames(const ParsedFile &file, Library &lib, IoIssues &issues)
{
  bool full = false;
  const auto add = [&](IdDomain domain, const FileName &n, uint64_t id) {
    if (!lib.runtime().nameOf(domain, id) && !lib.fileNames().add(domain, n.text)) {
      full = true;
    }
  };
  for (const FileName &n : file.names) {
    if (!n.isRaw && n.asClient) {
      add(IdDomain::Client, n, n.client.value);
    }
    if (!n.isRaw && n.asType) {
      add(IdDomain::Type, n, n.type.value);
    }
  }
  if (full) {
    issues.add(BlockId(), {},
               Status::error(StatusCode::OutOfRange,
                             "too many names to keep: a save will write some as ids"));
  }
}

struct Plan {
  BlockId id;
  bool aliased;
  BlockId stored;
  int record;
};

/* The records by the id they would have here, the one that is stored under it first. */
Status sortRecords(const ParsedFile &file, const ClientRegistry &clients, Vector<Plan> &plans)
{
  for (int i = 0; i < int(file.records.size()); i++) {
    const BlockId stored = file.records[i].id;
    const BlockId id = clients.canonical(stored);
    plans.append(Plan{id, id != stored, stored, i});
  }
  std::sort(first(plans), last(plans), [](const Plan &a, const Plan &b) {
    return std::tie(a.id, a.aliased, a.stored) < std::tie(b.id, b.aliased, b.stored);
  });
  for (int i = 1; i < int(plans.size()); i++) {
    if (plans[i - 1].stored == plans[i].stored) {
      const std::string name(file.records[plans[i].record].name.substr(0, 64));
      return Status::error(StatusCode::InvalidArgument,
                           ("the library section: two blocks have one id ('" + name + "')").c_str());
    }
  }
  return Status();
}

/* Ids nothing in the library or the file has. */
class IdSource {
 public:
  explicit IdSource(Library &lib) : lib_(lib) {}
  /* False if it is null or in use. */
  bool take(BlockId id)
  {
    return !id.isNull() && !lib_.find(id) && taken_.insert(id).second;
  }
  Result<BlockId> fresh(ClientId client)
  {
    for (int attempt = 0; attempt < 4; attempt++) {
      const BlockId id{client, lib_.runtime().newLocalId()};
      if (take(id)) {
        return id;
      }
    }
    return Status::error(StatusCode::Internal, "the GUID source keeps returning ids in use");
  }

 private:
  Library &lib_;
  std::set<BlockId> taken_;
};

/* Two records become one through a client alias: the one under the current name keeps the id,
 * and the others get fresh ones, which `moves` has. */
Status planRecordIds(ParsedFile &file, Library &lib, IdMap &moves, IoIssues &issues)
{
  Vector<Plan> plans;
  CDCC_TRY(sortRecords(file, lib.runtime().ext<ClientRegistry>(), plans));
  IdSource ids(lib);
  for (const Plan &p : plans) {
    ids.take(p.id);
  }
  for (int i = 0; i < int(plans.size()); i++) {
    const Plan &p = plans[i];
    ParsedRecord &r = file.records[p.record];
    if (i == 0 || plans[i - 1].id != p.id) {
      r.id = p.id;
      continue;
    }
    Result<BlockId> id = ids.fresh(p.id.client);
    if (!id.ok()) {
      return id.status();
    }
    r.id = *id;
    moves.add(p.stored, r.id);
    issues.add(r.id, r.name,
               Status::error(StatusCode::AlreadyExists,
                             "another block has its id, under its client's current name: it "
                             "got a new one"));
  }
  return Status();
}

Status planAppendIds(ParsedFile &file, Library &lib, RekeyFn &rekey, IdMap &moves)
{
  Vector<Plan> plans;
  CDCC_TRY(sortRecords(file, lib.runtime().ext<ClientRegistry>(), plans));
  const ClientId own = ClientId::of(kCoredccClient);
  IdSource ids(lib);
  std::set<std::string> refused;
  for (const Plan &p : plans) {
    ParsedRecord &r = file.records[p.record];
    BlockId to;
    if (p.id.client == own) {
      Result<BlockId> id = ids.fresh(own);
      if (!id.ok()) {
        return id.status();
      }
      to = *id;
    }
    else {
      to = rekey(r, p.id);
      if (lib.runtime().hasStickyError()) {
        return lib.runtime().takeStickyError();
      }
      to = to.isNull() ? to : lib.runtime().ext<ClientRegistry>().canonical(to);
      if (to.client != p.id.client || !ids.take(to)) {
        refused.insert(r.type.empty() ? "(no type)" : std::string(r.type));
        continue;
      }
    }
    r.id = to;
    moves.add(p.stored, to);
    moves.add(p.id, to); /* the first of those with one id here */
  }
  if (refused.empty()) {
    return Status();
  }
  std::string list;
  for (const std::string &type : refused) {
    list += (list.empty() ? "" : ", ") + type;
  }
  return Status::error(StatusCode::FailedPrecondition,
                       ("blocks of these types can't get ids here: " + list).c_str());
}

/* Gives every reference its target, finds the missing blocks, and checks the limits. */
Status placeRefs(ParsedFile &file, Library &lib, const LoadLimits &limits, const IdMap &moves)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  const auto place = [&](RefCell &cell) {
    if (cell.isNull()) {
      return;
    }
    const BlockId id = clients.canonical(cell.target);
    BlockId to = moves.find(cell.target);
    to = to.isNull() ? moves.find(id) : to;
    cell.target = to.isNull() ? id : to;
    if (isStrong(cell.kind)) {
      file.missing.append(cell.target);
    }
  };
  for (RefCell &cell : file.rows) {
    place(cell);
  }
  for (ParsedSlot &s : file.slots) {
    place(s.cell);
  }
  Vector<BlockId> records;
  for (const ParsedRecord &r : file.records) {
    records.append(r.id);
  }
  std::sort(first(records), last(records));
  std::sort(first(file.missing), last(file.missing));
  int kept = 0;
  for (int i = 0; i < int(file.missing.size()); i++) {
    const BlockId id = file.missing[i];
    if ((kept == 0 || file.missing[kept - 1] != id) &&
        !std::binary_search(first(records), last(records), id) && !lib.find(id))
    {
      file.missing[kept++] = id;
    }
  }
  file.missing.resize(kept);

  const uint64_t blocks = uint64_t(file.records.size()) + uint64_t(kept);
  if (blocks > limits.maxBlocks) {
    return Status::error(StatusCode::OutOfRange,
                         ("the library section: it needs more than " +
                          std::to_string(limits.maxBlocks) + " records")
                             .c_str());
  }
  if (!file.budget.take(uint64_t(kept) * kRecordCost)) {
    return Status::error(StatusCode::OutOfRange,
                         "the library section: it needs more memory than allowed");
  }
  return probeMemory(blocks * kRecordCost);
}

} // namespace

Status planIds(ParsedFile &file, Library &lib, const LoadLimits &limits, IoIssues &issues)
{
  CDCC_TRY(checkNames(file, lib.runtime(), lib.fileNames()));
  IdMap moves;
  CDCC_TRY(planRecordIds(file, lib, moves, issues));
  CDCC_TRY(placeRefs(file, lib, limits, moves));
  addNames(file, lib, issues);
  return Status();
}

Status planAppend(ParsedFile &file,
                  Library &lib,
                  const LoadLimits &limits,
                  RekeyFn rekey,
                  IdMap &ids,
                  IoIssues &issues)
{
  CDCC_TRY(checkNames(file, lib.runtime(), lib.fileNames()));
  CDCC_TRY(planAppendIds(file, lib, rekey, ids));
  CDCC_TRY(placeRefs(file, lib, limits, ids));
  addNames(file, lib, issues);
  return Status();
}

} // namespace coredcc::detail::libio
