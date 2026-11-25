#include "coredcc/graph/dependents.h"

#include "coredcc/graph/frame.h"
#include "coredcc/lib/registry.h"

namespace coredcc::graph {

/* ---- inputs ---- */

InputLink::~InputLink()
{
  move(nullptr);
  detachAll(consumers);
}

int InputLink::index() const
{
  return int(this - frame->links.begin());
}

void InputLink::move(InputLink **list)
{
  if (head == list) {
    return;
  }
  if (head) {
    (prev ? prev->next : *head) = next;
    if (next) {
      next->prev = prev;
    }
  }
  head = list;
  prev = nullptr;
  next = list ? *list : nullptr;
  if (next) {
    next->prev = this;
  }
  if (list) {
    *list = this;
  }
}

void InputLink::detachAll(InputLink *&list)
{
  while (InputLink *l = list) {
    list = l->next;
    l->head = nullptr;
    l->next = l->prev = nullptr;
  }
}

void Dependents::setSource(Frame &f, int input, const Vertex &source, KernelState *kernel)
{
  InputLink &l = f.links[input];
  l.frame = &f;
  l.move(kernel       ? &kernel->consumers :
         source.frame ? &source.frame->links[source.index].consumers :
                        nullptr);
}

/* ---- frames ---- */

void Dependents::addFrame(Frame &f)
{
  f.deps = this;
  f.nextOfDef = framesOf(f.defSerial);
  if (f.nextOfDef) {
    f.nextOfDef->prevOfDef = &f;
  }
  frames_.add_overwrite(f.defSerial, &f);
}

void Dependents::removeFrame(Frame &f)
{
  if (f.nextOfDef) {
    f.nextOfDef->prevOfDef = f.prevOfDef;
  }
  if (f.prevOfDef) {
    f.prevOfDef->nextOfDef = f.nextOfDef;
  }
  else if (f.nextOfDef) {
    frames_.add_overwrite(f.defSerial, f.nextOfDef);
  }
  else {
    frames_.remove(f.defSerial);
  }
}

/* ---- blocks ---- */

void Dependents::setBlocks(KernelState &k, ClientRegistry &clients)
{
  const Trace::Blocks *t = k.trace.more.get();
  const int used = t ? int(t->blocks.size()) : 0;
  const int n = used + (t ? int(t->queries.size()) : 0);
  const auto block = [&](int i) {
    return clients.canonical(i < used ? t->blocks[i].id : t->queries[i - used].id);
  };
  const auto aspects = [&](int i) { return i < used ? t->blocks[i].aspects : kAllAspects; };
  bool same = n == k.blockLinks.size();
  for (int i = 0; same && i < n; i++) {
    same = k.blockLinks[i].block == block(i) && k.blockLinks[i].aspects == aspects(i);
  }
  if (same) {
    return;
  }
  for (BlockLink &l : k.blockLinks) {
    unlink(l);
  }
  k.blockLinks.reset(n);
  for (int i = 0; i < n; i++) {
    BlockLink &l = k.blockLinks[i];
    l.kernel = &k;
    l.block = block(i);
    l.aspects = aspects(i);
    l.next = kernelsOf(l.block);
    if (l.next) {
      l.next->prev = &l;
    }
    blocks_.add_overwrite(l.block, &l);
  }
}

void Dependents::unlink(BlockLink &l)
{
  if (l.next) {
    l.next->prev = l.prev;
  }
  if (l.prev) {
    l.prev->next = l.next;
  }
  else if (l.next) {
    blocks_.add_overwrite(l.block, l.next);
  }
  else {
    blocks_.remove(l.block);
  }
}

void Dependents::removeKernel(KernelState &k)
{
  for (BlockLink &l : k.blockLinks) {
    unlink(l);
  }
  k.blockLinks.reset();
  if (k.foreach) {
    foreachs.remove(&k);
  }
}

} // namespace coredcc::graph
