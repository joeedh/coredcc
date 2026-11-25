/* The built-in node types that make and read tables (graph/foreach.h). */
#include "coredcc/graph/builtin_nodes.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/node_type.h"

#include <string>

namespace coredcc {

namespace {

constexpr int64_t kMaxRange = int64_t(1) << 24;

/* The table at an input; an empty one for null. */
Result<const Elements *> tableAt(const NodeExecContext &ctx, std::string_view input)
{
  static const Elements none;
  const PayloadRef &ref = ctx.input(input)->asPayload();
  const Elements *t = ref ? ref.get<Elements>() : &none;
  if (!t) {
    return Status::error(StatusCode::FailedPrecondition,
                         "the table's blocks aren't there any more");
  }
  CDCC_TRY(t->validate());
  return t;
}

Status noColumn(const Elements &t, const std::string &name)
{
  std::string text = "the table has no column '" + name + "'";
  for (int i = 0; i < int(t.columns.size()) && i < 8; i++) {
    text += (i ? ", '" : "; it has '") + std::string(t.columns[i].name.c_str()) + "'";
  }
  return Status::error(StatusCode::NotFound, text.c_str());
}

Status setTable(NodeExecContext &ctx, Elements &&t)
{
  return ctx.setOutput("elements", GraphValue::ofPayload(makePayload(std::move(t))));
}

Status range(NodeExecContext &ctx)
{
  const int64_t n = ctx.input("count")->asInt();
  if (n < 0 || n > kMaxRange) {
    return Status::error(StatusCode::OutOfRange,
                         ("the count is " + std::to_string(n) + ", and can be 0 to " +
                          std::to_string(kMaxRange))
                             .c_str());
  }
  Elements t;
  Elements::Column index, factor;
  index.name = "index";
  factor.name = "factor";
  for (int64_t i = 0; i < n; i++) {
    t.keys.append(i);
    index.values.append(GraphValue::ofInt(i));
    factor.values.append(GraphValue::ofFloat(n > 1 ? double(i) / double(n - 1) : 0));
  }
  t.columns.append(std::move(index));
  t.columns.append(std::move(factor));
  return setTable(ctx, std::move(t));
}

Status count(NodeExecContext &ctx)
{
  Result<const Elements *> t = tableAt(ctx, "elements");
  return t.ok() ? ctx.setOutput("count", GraphValue::ofInt((*t)->size())) : t.status();
}

Status value(NodeExecContext &ctx)
{
  Result<const Elements *> t = tableAt(ctx, "elements");
  if (!t.ok()) {
    return t.status();
  }
  const std::string name = ctx.param("column")->asString().c_str();
  const Elements::Column *c = (*t)->column(name);
  if (!c) {
    return noColumn(**t, name);
  }
  const int64_t row = ctx.input("index")->asInt();
  if (row < 0 || row >= (*t)->size()) {
    return Status::error(StatusCode::OutOfRange,
                         ("the index is " + std::to_string(row) + ", and the table has " +
                          std::to_string((*t)->size()) + " rows")
                             .c_str());
  }
  const SocketTypeRegistry &types = ctx.runtime().ext<SocketTypeRegistry>();
  const GraphValue &v = c->values[int(row)];
  const SocketType &to = *ctx.type().outputs[0].type;
  if (to.holds(v)) {
    return ctx.setOutput(0, v);
  }
  const SocketType *from = types.typeOf(v);
  if (!from) {
    return Status::error(StatusCode::TypeMismatch,
                         ("column '" + name + "' has no value").c_str());
  }
  GraphValue out;
  CDCC_TRY(types.convert(*from, to, v, out));
  return ctx.setOutput(0, std::move(out));
}

/* Renames a column, over one that has the new name. */
Status rename(NodeExecContext &ctx)
{
  Result<const Elements *> t = tableAt(ctx, "elements");
  if (!t.ok()) {
    return t.status();
  }
  const std::string from = ctx.param("from")->asString().c_str();
  const std::string to = ctx.param("to")->asString().c_str();
  if (!(*t)->column(from)) {
    return noColumn(**t, from);
  }
  Elements out;
  out.keys = (*t)->keys;
  for (const Elements::Column &c : (*t)->columns) {
    if (from == c.name.c_str()) {
      out.columns.append(c);
      out.columns.last().name = to.c_str();
    } else if (to != c.name.c_str()) {
      out.columns.append(c);
    }
  }
  return setTable(ctx, std::move(out));
}

/* a's rows, with b's columns too: those of the row of the same key. */
Status combine(NodeExecContext &ctx)
{
  Result<const Elements *> a = tableAt(ctx, "a"), b = tableAt(ctx, "b");
  CDCC_TRY(a.status());
  CDCC_TRY(b.status());
  litestl::util::Map<int64_t, int> rows;
  for (int i = 0; i < (*b)->size(); i++) {
    rows.add((*b)->keys[i], i);
  }
  litestl::util::Vector<int> from;
  for (int64_t key : (*a)->keys) {
    const int *row = rows.lookup_ptr(key);
    if (!row) {
      return Status::error(StatusCode::NotFound,
                           ("b has no element " + std::to_string(key)).c_str());
    }
    from.append(*row);
  }
  Elements out = **a;
  for (const Elements::Column &c : (*b)->columns) {
    if ((*a)->column(c.name.c_str())) {
      continue;
    }
    Elements::Column made;
    made.name = c.name;
    for (int row : from) {
      made.values.append(c.values[row]);
    }
    out.columns.append(std::move(made));
  }
  return setTable(ctx, std::move(out));
}

} // namespace

void detail::addElementNodes(Runtime &rt, NodeTypeRegistry &reg)
{
  NodeTypeDef r("elements.range");
  r.uiName("Range")
      .input(PropertyDef::intProp("count")
                 .range(0, double(kMaxRange))
                 .defaultValue(PropValue::ofInt(10)))
      .output("elements", "elements");
  addBuiltin<range>(rt, reg, r, "elements.range");

  NodeTypeDef n("elements.count");
  n.uiName("Element Count").payloadInput("elements", "elements").output("count", "int");
  addBuiltin<count>(rt, reg, n, "elements.count");

  for (const char *type :
       {"bool", "int", "float", "vec2", "vec3", "vec4", "mat4", "quat", "string"})
  {
    const std::string name = std::string("elements.value.") + type;
    NodeTypeDef v(name);
    v.uiName("Element Value")
        .payloadInput("elements", "elements")
        .input(PropertyDef::intProp("index"))
        .param(PropertyDef::stringProp("column"))
        .output("value", type);
    addBuiltin<value>(rt, reg, v, name);
  }

  NodeTypeDef m("elements.rename");
  m.uiName("Rename Column")
      .payloadInput("elements", "elements")
      .param(PropertyDef::stringProp("from"))
      .param(PropertyDef::stringProp("to"))
      .output("elements", "elements");
  addBuiltin<rename>(rt, reg, m, "elements.rename");

  NodeTypeDef c("elements.combine");
  c.uiName("Combine Elements")
      .payloadInput("a", "elements")
      .payloadInput("b", "elements")
      .output("elements", "elements");
  addBuiltin<combine>(rt, reg, c, "elements.combine");
}

} // namespace coredcc
