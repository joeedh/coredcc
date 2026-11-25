/* Embedding check (Windows): coredcc's public headers, and the litestl headers they
 * include, must compile after <windows.h> without NOMINMAX and next to Unreal-style
 * macros, as they would inside an Unreal editor module. The templates are instantiated
 * too, so macro clashes inside them show up. */
#include <windows.h>
#define check(expr) ((void)(expr))
#define verify(expr) ((void)(expr))
#define ensure(expr) (!!(expr))
#undef TEXT
#define TEXT(x) L##x

#include "coredcc/api/attachments.h"
#include "coredcc/api/change_bus.h"
#include "coredcc/api/context.h"
#include "coredcc/api/library_io.h"
#include "coredcc/api/ref_text.h"
#include "coredcc/api/resolve.h"
#include "coredcc/api/static_resolve.h"
#include "coredcc/api/struct.h"
#include "coredcc/base/assert.h"
#include "coredcc/bvh/any_source.h"
#include "coredcc/bvh/geometry_bvh.h"
#include "coredcc/command/attachment_commands.h"
#include "coredcc/command/block_commands.h"
#include "coredcc/command/block_edits.h"
#include "coredcc/command/command.h"
#include "coredcc/command/executor.h"
#include "coredcc/command/history_block.h"
#include "coredcc/command/last_used.h"
#include "coredcc/command/library_commands.h"
#include "coredcc/command/literal.h"
#include "coredcc/command/set_property.h"
#include "coredcc/command/snapshot_undo.h"
#include "coredcc/command/undo_strategies.h"
#include "coredcc/base/block_id.h"
#include "coredcc/base/bytes.h"
#include "coredcc/base/flags.h"
#include "coredcc/base/handle_pool.h"
#include "coredcc/base/id.h"
#include "coredcc/base/iface/iface.h"
#include "coredcc/base/iface/owned.h"
#include "coredcc/base/iface/registry.h"
#include "coredcc/base/json_writer.h"
#include "coredcc/base/name_table.h"
#include "coredcc/base/runtime.h"
#include "coredcc/base/signal.h"
#include "coredcc/base/status.h"
#include "coredcc/base/text.h"
#include "coredcc/base/utf8.h"
#include "coredcc/graph/api.h"
#include "coredcc/graph/array.h"
#include "coredcc/graph/commands.h"
#include "coredcc/graph/def.h"
#include "coredcc/graph/def_io.h"
#include "coredcc/graph/dependents.h"
#include "coredcc/graph/evaluator.h"
#include "coredcc/graph/exec_context.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/frame.h"
#include "coredcc/graph/instance.h"
#include "coredcc/graph/interior.h"
#include "coredcc/graph/node_type.h"
#include "coredcc/graph/ops.h"
#include "coredcc/graph/payload.h"
#include "coredcc/graph/provider.h"
#include "coredcc/graph/socket_key.h"
#include "coredcc/graph/socket_type.h"
#include "coredcc/graph/system.h"
#include "coredcc/graph/topology.h"
#include "coredcc/graph/trace.h"
#include "coredcc/graph/value.h"
#include "coredcc/layout/builder.h"
#include "coredcc/layout/draft.h"
#include "coredcc/layout/layout.h"
#include "coredcc/layout/validate.h"
#include "coredcc/layout/xml.h"
#include "coredcc/lib/attachment.h"
#include "coredcc/draw/view.h"
#include "coredcc/lib/block.h"
#include "coredcc/lib/bag_refs.h"
#include "coredcc/lib/block_io.h"
#include "coredcc/lib/duplicate.h"
#include "coredcc/lib/id_key.h"
#include "coredcc/lib/id_map.h"
#include "coredcc/lib/io/library_io.h"
#include "coredcc/lib/io/load.h"
#include "coredcc/lib/io/pack.h"
#include "coredcc/lib/library.h"
#include "coredcc/lib/pin.h"
#include "coredcc/lib/provider.h"
#include "coredcc/lib/registry.h"
#include "coredcc/lib/type_names.h"
#include "coredcc/parse/invocation.h"
#include "coredcc/parse/path.h"
#include "coredcc/parse/path_cache.h"
#include "coredcc/props/bag.h"
#include "coredcc/props/def.h"
#include "coredcc/props/kind.h"
#include "coredcc/props/ref.h"
#include "coredcc/props/units.h"
#include "coredcc/props/value.h"
#include "coredcc/props/value_io.h"
#include "coredcc/scene/builtins.h"
#include "coredcc/scene/capabilities.h"
#include "coredcc/scene/evaluated.h"
#include "coredcc/scene/object.h"
#include "coredcc/scene/scene.h"
#include "coredcc/scene/solve.h"
#include "coredcc/scene/transform_registry.h"

#include "test_util.h"

test_init;

namespace lbt = litestl::binding::types;

struct Sized : coredcc::IfaceRef {
  int size() const
  {
    return CDCC_CALL(size);
  }
  double weight() const
  {
    return CDCC_HAS(weight) ? CDCC_CALL(weight) : 1.0;
  }
  CDCC_INTERFACE(Sized, "embed.Sized")
  {
    BIND_STRUCT_METHOD(st, size, MARGS());
    BIND_OPTIONAL_METHOD(st, weight, MARGS());
  }
};

struct Box {
  int n = 4;
  int size() const
  {
    return n;
  }
  static lbt::Struct<Box> *defineBindings()
  {
    auto *st = new lbt::Struct<Box>("embed::Box", sizeof(Box));
    BIND_STRUCT_MEMBER(st, n);
    BIND_STRUCT_METHOD(st, size, MARGS());
    return st;
  }
};

int main()
{
#if !defined(min) || !defined(max)
#error "windows.h min/max macros are not defined, so this test proves nothing"
#endif
  const wchar_t *unrealText = TEXT("text");
  test_assert(unrealText[0] == L't');
  {
    coredcc::Runtime rt;
    test_assert((coredcc::registerImpl<Sized, Box>(rt, "embed.Box").ok()));
    Box box;
    Sized s = coredcc::query<Sized>(rt, coredcc::TypeId::of("embed.Box"), &box);
    test_assert(s.size() == 4 && s.weight() == 1.0);
    coredcc::HandlePool<int> pool;
    coredcc::Signal<int> signal;
    coredcc::Library lib(&rt);
    coredcc::Context ctx(&rt, &lib, coredcc::PointerRef());
    test_assert(!coredcc::getValue(ctx, "x").ok());
    test_assert(rt.ext<coredcc::ChangeBus>().flush() == 0);
    coredcc::LinearHistory history;
    coredcc::CommandExecutor ex(&rt, &history);
    test_assert(coredcc::registerBlockCommands(rt).ok());
    ctx.setExecutor(&ex);
    test_assert(coredcc::invoke(ctx, "core.block.purge").ok());
    test_assert(rt.ext<coredcc::PathCache>().get("a.b[\"c\"]").ok());
    test_assert(coredcc::coerce(coredcc::PropertyDef::floatProp("f").range(0, 1),
                                coredcc::PropValue::ofFloat(2))
                    .ok());
    coredcc::TriangleSource<float> triangles;
    triangles.append({0, 0, 0}, {1, 0, 0}, {0, 1, 0});
    coredcc::GeometryBvh<coredcc::TriangleSource<float>> bvh;
    test_assert(bvh.build(triangles).ok());
    coredcc::Ray ray;
    ray.origin = {0.25, 0.25, -1};
    ray.dir = {0, 0, 1};
    test_assert(bvh.rayClosest(ray) && bvh.rayAny(ray) && bvh.nearest(ray.origin, 2));
    coredcc::LayoutBuilder layout;
    layout.root().panel("Panel").prop("a.b").visibleIf("!c");
    test_assert(coredcc::layoutToJson(*layout.finish()).size() > 0);
    test_assert(coredcc::parseLayout("<layout/>").ok());
    test_assert(!coredcc::AnyBvh().build(rt, coredcc::AnyBvhSource()).ok());
    test_assert(coredcc::registerTransformTypes(rt).ok());
    test_assert(!coredcc::ViewMath().set(coredcc::View()).ok());
    test_assert(rt.validateAll().ok());
  }
  return test_end();
}
