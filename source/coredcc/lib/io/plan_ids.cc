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

struct Move {
  BlockId from;
  BlockId to;
};

/* Gives each record its id, and returns the ones that had to change. Two records become one
 * through a client alias: the one under the current name keeps the id. */
Status planRecordIds(ParsedFile &file, Library &lib, Vector<Move> &moves, IoIssues &issues)
{
  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  struct Plan {
    BlockId id;
    bool aliased;
    BlockId stored;
    int record;
  };
  Vector<Plan> plans;
  Vector<BlockId> taken;
  for (int i = 0; i < int(file.records.size()); i++) {
    const BlockId stored = file.records[i].id;
    const BlockId id = clients.canonical(stored);
    plans.append(Plan{id, id != stored, stored, i});
    taken.append(id);
  }
  std::sort(first(plans), last(plans), [](const Plan &a, const Plan &b) {
    return std::tie(a.id, a.aliased, a.stored) < std::tie(b.id, b.aliased, b.stored);
  });
  std::sort(first(taken), last(taken));
  std::set<BlockId> fresh;
  for (int i = 0; i < int(plans.size()); i++) {
    const Plan &p = plans[i];
    ParsedRecord &r = file.records[p.record];
    if (i == 0 || plans[i - 1].id != p.id) {
      r.id = p.id;
      continue;
    }
    if (plans[i - 1].stored == p.stored) {
      return Status::error(StatusCode::InvalidArgument,
                           ("the library section: two blocks have one id ('" +
                            std::string(r.name.substr(0, 64)) + "')")
                               .c_str());
    }
    for (int attempt = 0; attempt < 4; attempt++) {
      const BlockId id{p.id.client, lib.runtime().newLocalId()};
      if (!id.isNull() && !std::binary_search(first(taken), last(taken), id) &&
          fresh.insert(id).second)
      {
        r.id = id;
        break;
      }
    }
    if (r.id == p.stored) {
      return Status::error(StatusCode::Internal, "the GUID source keeps returning ids in use");
    }
    moves.append(Move{p.stored, r.id});
    issues.add(r.id, r.name,
               Status::error(StatusCode::AlreadyExists,
                             "another block has its id, under its client's current name: it "
                             "got a new one"));
  }
  std::sort(first(moves), last(moves),
            [](const Move &a, const Move &b) { return a.from < b.from; });
  return Status();
}

} // namespace

Status planIds(ParsedFile &file, Library &lib, const LoadLimits &limits, IoIssues &issues)
{
  CDCC_TRY(checkNames(file, lib.runtime(), lib.fileNames()));
  Vector<Move> moves;
  CDCC_TRY(planRecordIds(file, lib, moves, issues));

  const ClientRegistry &clients = lib.runtime().ext<ClientRegistry>();
  const auto place = [&](RefCell &cell) {
    if (cell.isNull()) {
      return;
    }
    const auto move = std::lower_bound(first(moves), last(moves), cell.target,
                                       [](const Move &m, const BlockId &id) { return m.from < id; });
    cell.target = move != last(moves) && move->from == cell.target ?
                      move->to :
                      clients.canonical(cell.target);
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
        !std::binary_search(first(records), last(records), id))
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
  CDCC_TRY(probeMemory(blocks * kRecordCost));
  addNames(file, lib, issues);
  return Status();
}

} // namespace coredcc::detail::libio
