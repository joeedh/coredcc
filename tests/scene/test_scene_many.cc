/* Scene::removeMany, putBack and addMany, and what commands ask about many objects at
 * once, against one object at a time. */
#include "command_doc.h"

#include "coredcc/scene/state_cache.h"

#include <algorithm>
#include <string>

test_init;

using namespace scene_test;

namespace {

std::vector<BlockId> cellIds(CommandDoc &d)
{
  std::vector<BlockId> out;
  for (const RefCell &c : d.sc().cells()) {
    out.push_back(c.target);
  }
  return out;
}

/* The place, or -1. */
template <class T> int placeOf(const std::vector<T> &all, const T &x)
{
  for (size_t i = 0; i < all.size(); i++) {
    if (all[i] == x) {
      return int(i);
    }
  }
  return -1;
}

template <class T> bool has(const std::vector<T> &all, const T &x)
{
  return placeOf(all, x) >= 0;
}

bool indexIsRight(CommandDoc &d, const std::vector<BlockHandle> &all)
{
  const std::vector<BlockId> cells = cellIds(d);
  bool ok = true;
  for (BlockHandle h : all) {
    if (d.lib->get(h)) {
      ok = ok && d.sc().indexOf(*d.lib, h) == placeOf(cells, d.idOf(h));
    }
  }
  return ok;
}

std::span<const Scene::Place> spanOf(const Vector<Scene::Place> &places)
{
  return {places.data(), size_t(places.size())};
}

void testRemoveMany()
{
  const int rounds = 300 * coredcc_test::testScale();
  CommandDoc d;
  Rand r(81);
  std::vector<BlockHandle> all;
  int behind = 0, atIndex = 0, refused = 0, intact = 0;
  for (int round = 0; round < rounds; round++) {
    while (d.sc().cells().size() < 16) {
      all.push_back(d.object("o"));
    }
    const std::vector<BlockId> before = cellIds(d);
    std::vector<BlockId> ids, sorted, left;
    for (int i = int(r.below(8)); i >= 0; i--) {
      /* The last one, often: the index keeps what is before it. */
      ids.push_back(r.below(3) ? before[r.below(uint32_t(before.size()))]
                               : before.back());
    }
    for (const BlockId &id : before) {
      (has(ids, id) ? sorted : left).push_back(id);
    }
    if (r.coin()) {
      test_assert(indexIsRight(d, all)); /* or it is as the last round left it */
    }

    /* One that isn't a member, and nothing is removed. */
    std::vector<BlockId> bad = ids;
    bad.push_back(r.coin() ? d.idOf(d.loose("other")) : BlockId());
    test_assert(
        isError(d.sc().removeMany(*d.lib, bad).status(), StatusCode::NotFound, ""));
    test_assert(cellIds(d) == before);

    Result<Vector<Scene::Place>> places = d.sc().removeMany(*d.lib, ids);
    test_assert(places.ok() && places->size() == sorted.size());
    test_assert(cellIds(d) == left && indexIsRight(d, all));
    for (int i = 0; i < int(sorted.size()); i++) {
      const Scene::Place &p = (*places)[i];
      test_assert(p.id == sorted[size_t(i)] && before[size_t(p.index)] == p.id);
      test_assert(p.previous == (p.index ? before[size_t(p.index) - 1] : BlockId()));
    }
    if (r.coin()) {
      must(d.sc().putBack(*d.lib, spanOf(*places)));
      test_assert(cellIds(d) == before && indexIsRight(d, all));
      intact++;
      continue;
    }

    /* Others go and come meanwhile, and one of them is there already. */
    for (int k = int(r.below(6)); k > 0 && !left.empty(); k--) {
      const size_t at = r.below(uint32_t(left.size()));
      must(d.sc().remove(*d.lib, left[at]).status());
      left.erase(left.begin() + ptrdiff_t(at));
    }
    const BlockHandle added = d.loose("added");
    all.push_back(added);
    const size_t at = r.below(uint32_t(left.size() + 1));
    must(d.sc().add(*d.lib, added, int(at)));
    left.insert(left.begin() + ptrdiff_t(at), d.idOf(added));
    BlockId early;
    if (r.below(3) == 0) {
      early = sorted[r.below(uint32_t(sorted.size()))];
      must(d.sc().add(*d.lib, d.lib->find(early)));
      left.push_back(early);
      refused++;
    }
    const Status s = d.sc().putBack(*d.lib, spanOf(*places));
    test_assert(early.isNull() ? s.ok() : isError(s, StatusCode::AlreadyExists, ""));
    const std::vector<BlockId> after = cellIds(d);
    test_assert(after.size() == left.size() + sorted.size() - (early.isNull() ? 0 : 1));
    test_assert(indexIsRight(d, all));
    /* Those that stayed are in their order, and each is behind the one before it, if that
     * is there. */
    std::vector<BlockId> stayed;
    for (const BlockId &id : after) {
      if (has(left, id)) {
        stayed.push_back(id);
      }
    }
    test_assert(stayed == left);
    const auto where = [&](const BlockId &id) { return placeOf(after, id); };
    for (const Scene::Place &p : *places) {
      if (p.id == early) {
        continue;
      }
      if (!p.previous.isNull() && has(after, p.previous)) {
        behind++;
        test_assert(where(p.id) == where(p.previous) + 1);
      } else {
        /* Nothing is put in front of it but what comes behind an earlier one that moved.
         */
        atIndex++;
        const int here = where(p.id), last = int(after.size()) - 1;
        test_assert(&p != places->data() || here >= std::min(p.index, last));
        test_assert(here <= p.index + int(sorted.size()));
      }
    }
  }
  fprintf(stderr,
          "put back: %d at once, %d behind, %d at their index, %d refused\n",
          intact,
          behind,
          atIndex,
          refused);
  test_assert(intact > 50 && behind > 100 && atIndex > 20 && refused > 20);
}

void testPutBack()
{
  CommandDoc d;
  std::vector<BlockHandle> h;
  for (int i = 0; i < 6; i++) {
    h.push_back(d.object(std::string(1, char('a' + i)).c_str()));
  }
  const auto names = [&]() {
    std::string out;
    for (const RefCell &c : d.sc().cells()) {
      out += d.lib->get(d.lib->find(c.target))->name().c_str();
    }
    return out;
  };
  const auto remove = [&](std::initializer_list<int> which) {
    std::vector<BlockId> out;
    for (int i : which) {
      out.push_back(d.idOf(h[size_t(i)]));
    }
    Result<Vector<Scene::Place>> places = d.sc().removeMany(*d.lib, out);
    must(places.status());
    return std::move(places).value();
  };
  test_assert(remove({}).size() == 0);
  /* Without the one before it, at its place: counting those put back. */
  Vector<Scene::Place> places = remove({1, 2, 4});
  test_assert(names() == "adf");
  remove({0});
  must(d.sc().putBack(*d.lib, spanOf(places)));
  test_assert(names() == "debcf"); /* e behind d, and c behind b, which had place 1 */
  must(d.sc().putBack(*d.lib, {}));

  /* At the end, when there are fewer. */
  places = remove({5});
  remove({1, 2, 4});
  must(d.sc().putBack(*d.lib, spanOf(places)));
  test_assert(names() == "df");

  /* One whose record has gone is left out, and what was behind it goes to its place. */
  must(d.sc().add(*d.lib, h[1]));
  must(d.sc().add(*d.lib, h[2]));
  places = remove({1, 2});
  must(d.lib->markDeleted(h[1]));
  must(d.lib->destroyFromHistory(h[1]));
  test_assert(isError(
      d.sc().putBack(*d.lib, spanOf(places)), StatusCode::NotFound, "no such block"));
  test_assert(names() == "dfc");

  /* Places that name each other, and one that is there twice. */
  remove({2});
  Vector<Scene::Place> odd;
  odd.append(Scene::Place{d.idOf(h[2]), d.idOf(h[4]), 0});
  odd.append(Scene::Place{d.idOf(h[4]), d.idOf(h[2]), 0});
  odd.append(Scene::Place{d.idOf(h[2]), BlockId(), 0});
  test_assert(isError(
      d.sc().putBack(*d.lib, spanOf(odd)), StatusCode::AlreadyExists, "in the scene"));
  test_assert(names() == "dfce" && d.sc().indexOf(*d.lib, h[4]) == 3);
}

void testAddMany()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.loose("b"), c = d.loose("c"),
                    e = d.loose("e");
  const auto add = [&](std::initializer_list<BlockHandle> hs) {
    const std::vector<BlockHandle> v(hs);
    return d.sc().addMany(*d.lib, v);
  };
  must(add({}));
  const std::vector<BlockId> was = cellIds(d);
  test_assert(isError(add({b, a}), StatusCode::AlreadyExists, "'a'"));
  test_assert(isError(add({b, c, b}), StatusCode::AlreadyExists, "'b'"));
  test_assert(isError(add({b, BlockHandle()}), StatusCode::NotFound, "no such block"));
  test_assert(cellIds(d) == was);
  must(add({c, b}));
  test_assert(indexIsRight(d, {a, b, c, e}) && d.sc().indexOf(*d.lib, b) == 2);
  test_assert(d.sc().objects(*d.lib).size() == 3);
  must(d.sc().removeMany(*d.lib, was).status());
  must(add({e, a}));
  test_assert(indexIsRight(d, {a, b, c, e}) && d.sc().indexOf(*d.lib, a) == 3);
}

/* Records that went are looked for once. */
void testPruneRemoved()
{
  CommandDoc d;
  const BlockHandle a = d.object("a"), b = d.object("b");
  d.sc().pruneRemoved(*d.lib);
  const uint64_t count = d.lib->removedCount();
  must(d.lib->markDeleted(a));
  test_assert(d.lib->removedCount() == count); /* it can come back */
  must(d.lib->destroyFromHistory(a));
  test_assert(d.lib->removedCount() == count + 1 && d.sc().cells().size() == 2);
  d.sc().pruneRemoved(*d.lib);
  test_assert(d.sc().cells().size() == 1);
  {
    /* A host still holds the data: the record has gone all the same. */
    const LibraryPin pin(*d.lib, b, PinKind::Host);
    must(d.lib->markDeleted(b));
    must(d.lib->destroyFromHistory(b));
    test_assert(d.lib->removedCount() == count + 2 &&
                !d.lib->find(d.sc().cells()[0].target));
    d.sc().pruneRemoved(*d.lib);
    test_assert(d.sc().cells().size() == 0);
  }
  test_assert(d.lib->removedCount() == count + 2);
  /* A sweep. */
  d.loose("c");
  test_assert(*d.lib->collect() == 1 && d.lib->removedCount() == count + 3);
}

/* ---- states and ancestors ---- */

struct Naive {
  CommandDoc &d;

  BlockHandle parentOf(BlockHandle h)
  {
    const Transformable t = d.lib->query<Transformable>(h);
    if (!t) {
      return BlockHandle();
    }
    ObjectInfo info;
    ChannelBlock c;
    t.read(info, c);
    const BlockHandle p = info.parent.isNull() ? BlockHandle() : d.lib->find(info.parent);
    const Datablock *b = d.lib->get(p);
    return b && b->state() == BlockState::Live ? p : BlockHandle();
  }
  /* Those above it, the nearest first; itself is the last one on a cycle. */
  std::vector<BlockHandle> above(BlockHandle h)
  {
    std::vector<BlockHandle> out;
    for (BlockHandle p = parentOf(h); p && !has(out, p); p = parentOf(p)) {
      out.push_back(p);
    }
    return out;
  }
  bool onCycle(BlockHandle h)
  {
    return has(above(h), h);
  }
};

void testCaches()
{
  const int rounds = 150 * coredcc_test::testScale();
  Rand r(82);
  int cycles = 0, belowCycles = 0, within = 0, dead = 0;
  for (int round = 0; round < rounds; round++) {
    CommandDoc d;
    const BlockHandle shape = d.shapeBlock("shape", 1.0);
    std::vector<BlockHandle> all;
    for (int i = 0; i < 14; i++) {
      all.push_back(r.below(5) ? d.object("o") : d.hostObject("h", r.range(-3, 3)));
    }
    for (BlockHandle h : all) {
      const uint32_t pick = r.below(8);
      const BlockHandle p = pick == 0   ? BlockHandle()
                            : pick == 1 ? shape
                                        : all[r.below(uint32_t(all.size()))];
      if (d.lib->get(h)->type() == TypeId::of(kObjectType)) {
        const bool fbx = r.coin();
        d.setParent(h, p);
        d.obj(h).setTransformType(fbx ? kFbxTransform : kTrsTransform);
        d.obj(h).channels =
            fbx ? fbxChannels(r, *d.fbx, int(r.below(3)), r.scale(2, true), 2.0)
                : trsChannels(r, *d.trs, r.scale(2, true), 5.0);
      } else {
        d.host(h).info.parent = p ? d.idOf(p) : BlockId();
      }
    }
    if (r.coin()) {
      must(d.lib->markDeleted(all.back()));
      all.pop_back();
      dead++;
    }
    detail::HandleSet set;
    for (BlockHandle h : all) {
      if (r.below(5) == 0) {
        set.add(h, 0);
      }
    }
    Naive naive{d};
    detail::StateCache states(*d.lib, 0.0);
    detail::Ancestry ancestry(*d.lib, set);
    for (int k = 0; k < 40; k++) {
      const BlockHandle h = all[r.below(uint32_t(all.size()))];
      Result<ObjectRead> read = readObject(*d.lib, h);
      must(read.status());
      switch (r.below(3)) {
      case 0: {
        Result<TransformState> s = states.state(h), want = computeState(*d.lib, h, 0.0);
        test_assert(s.ok() && want.ok() && sameBits(*s, *want));
        break;
      }
      case 1: {
        Result<TransformState> s = states.stateAbove(h, *read),
                               want = computeParentState(*d.lib, h, 0.0);
        test_assert(s.ok() && want.ok() && sameBits(*s, *want));
        Result<TransformState> own = states.stateUnder(*read, *s);
        test_assert(own.ok() && sameBits(*own, *computeState(*d.lib, h, 0.0)));
        break;
      }
      default:
        test_assert(sameBits(*states.parentState(shape), TransformState()));
      }

      const std::vector<BlockHandle> above = naive.above(h);
      const bool cycle = naive.onCycle(h), in = set.lookup_ptr(h) != nullptr;
      bool belowCycle = false, inSet = false;
      BlockHandle outside = in ? BlockHandle() : h;
      for (BlockHandle p : above) {
        belowCycle = belowCycle || (!cycle && naive.onCycle(p));
        inSet = inSet || set.lookup_ptr(p);
        outside = outside || set.lookup_ptr(p) ? outside : p;
      }
      Result<bool> below = ancestry.below(h);
      test_assert(below.ok() && *below == (inSet && !cycle));
      Result<detail::Ancestry::Place> place = ancestry.place(h);
      test_assert(place.ok() && place->parent == naive.parentOf(h) &&
                  place->cycle == cycle);
      test_assert(place->within == (inSet || in) && place->outside == outside);
      test_assert(cycle || belowCycle || place->depth == int(above.size()));
      test_assert(!cycle || place->depth == 0);
      test_assert(!belowCycle ||
                  place->depth == ancestry.place(place->parent)->depth + 1);
      cycles += cycle;
      belowCycles += belowCycle;
      within += inSet;
    }
  }
  fprintf(stderr,
          "caches: %d on cycles, %d below, %d within, %d dead\n",
          cycles,
          belowCycles,
          within,
          dead);
  test_assert(cycles > 100 && belowCycles > 100 && within > 100 && dead > 10);
}

/* What can't be read is an error, not a state. */
void testCacheErrors()
{
  CommandDoc d;
  const BlockHandle h = d.hostObject("h"), a = d.object("a");
  d.setParent(a, h);
  d.host(h).onRead = [&](ObjectInfo &) { d.rt.setStickyError(boom("read")); };
  detail::StateCache states(*d.lib, 0.0);
  Result<ObjectRead> read = readObject(*d.lib, a);
  must(read.status());
  test_assert(isError(states.state(a).status(), StatusCode::Internal, "read"));
  test_assert(
      isError(states.stateAbove(a, *read).status(), StatusCode::Internal, "read"));
  test_assert(isError(states.parentState(h).status(), StatusCode::Internal, "read"));
  detail::HandleSet none;
  detail::Ancestry ancestry(*d.lib, none);
  test_assert(isError(ancestry.place(a).status(), StatusCode::Internal, "read"));
  test_assert(isError(ancestry.below(a).status(), StatusCode::Internal, "read"));
  test_assert(!d.rt.hasStickyError());
  d.host(h).onRead = nullptr;
  test_assert(states.state(a).ok() && ancestry.place(a)->depth == 1);
  d.addFnModel("test.fn", [&](const EvalCtx &, const TransformState &p) {
    d.rt.setStickyError(boom("model"));
    return p;
  });
  d.obj(a).setTransformType("test.fn");
  d.obj(a).channels = ChannelBlock(1);
  test_assert(isError(
      detail::StateCache(*d.lib, 0.0).state(a).status(), StatusCode::Internal, "model"));
  test_assert(!states.state(d.shapeBlock("shape", 1.0)).ok() &&
              !states.state(BlockHandle()).ok());
}

/* A child on a cycle that a deletion opens: each is placed under what its parent is by
 * then. */
void testDeleteCycles()
{
  const int rounds = 150 * coredcc_test::testScale();
  Rand r(83);
  int opened = 0, children = 0;
  for (int round = 0; round < rounds; round++) {
    CommandDoc d;
    std::vector<BlockHandle> all;
    for (int i = 0; i < 8; i++) {
      all.push_back(d.object("o", r.range(-4, 4), r.range(-4, 4)));
    }
    for (BlockHandle h : all) {
      if (r.below(6)) {
        d.setParent(h, all[r.below(8)]);
      }
    }
    Naive naive{d};
    const BlockHandle gone = all[r.below(8)], other = all[r.below(8)];
    opened += naive.onCycle(gone) || naive.onCycle(other);
    std::vector<std::pair<BlockHandle, mat4d>> was;
    for (BlockHandle h : all) {
      const BlockHandle p = naive.parentOf(h);
      if (h != gone && h != other && p && (p == gone || p == other)) {
        was.push_back({h, d.stateOf(h).world});
      }
    }
    d.select({gone, other});
    d.cycle(kDeleteObjectsCommand);
    for (const auto &[h, world] : was) {
      children++;
      test_assert(difference(d.stateOf(h).world, world) < 1e-9);
    }
  }
  fprintf(stderr, "deleted: %d on cycles, %d children\n", opened, children);
  test_assert(opened > 30 && children > 60);
}

} // namespace

int main()
{
  testRemoveMany();
  testPutBack();
  testAddMany();
  testPruneRemoved();
  testCaches();
  testCacheErrors();
  testDeleteCycles();
  return test_end();
}
