#include "coredcc/bind/script_table.h"

#include "coredcc/bind/detail.h"

#include <string>

namespace coredcc::script {

using detail::error;
using detail::failed;
using detail::ok;
using detail::view;
namespace lbt = litestl::binding::types;

const Elements *ScriptTable::get() const
{
  static const Elements none;
  return ref_ ? ref_.get<Elements>() : &none;
}

Elements *ScriptTable::edit()
{
  if (!ref_) {
    ref_ = makePayload(Elements());
  }
  return ref_.mutate<Elements>();
}

static Status gone()
{
  return error(StatusCode::FailedPrecondition, "the table's blocks aren't there any more");
}

Status ScriptTable::check() const
{
  const Elements *t = get();
  if (!t) {
    return gone();
  }
  CDCC_TRY(t->validate());
  for (const Elements::Column &c : t->columns) {
    for (int i = 0; i < t->size(); i++) {
      if (c.values[i].isNone()) {
        return error(StatusCode::InvalidArgument,
                     std::string("column '") + c.name.c_str() + "' of the table has no value in "
                                                                "row " +
                         std::to_string(i));
      }
    }
  }
  return Status();
}

static Status noRow(const Elements &t, int32_t row)
{
  return error(StatusCode::NotFound, "row " + std::to_string(row) + " of a table that has " +
                                         std::to_string(t.size()));
}

static Status noColumn(const string &name)
{
  return error(StatusCode::NotFound, std::string("the table has no column '") + name.c_str() + "'");
}

TableResult newTable()
{
  return ok<TableResult>(ScriptTable(makePayload(Elements())));
}

int32_t tableRows(const ScriptTable &table)
{
  const Elements *t = table.get();
  return t ? t->size() : 0;
}

int32_t tableColumns(const ScriptTable &table)
{
  const Elements *t = table.get();
  return t ? int32_t(t->columns.size()) : 0;
}

StringResult tableColumnName(const ScriptTable &table, int32_t column)
{
  if (column < 0 || column >= tableColumns(table)) {
    return failed<StringResult>(
        error(StatusCode::NotFound, "column " + std::to_string(column) + " of a table that has " +
                                        std::to_string(tableColumns(table))));
  }
  return ok<StringResult>(table.get()->columns[column].name);
}

IntResult tableKey(const ScriptTable &table, int32_t row)
{
  const Elements *t = table.get();
  if (!t) {
    return failed<IntResult>(gone());
  }
  return row >= 0 && row < t->size() ? ok<IntResult>(t->keys[row]) :
                                       failed<IntResult>(noRow(*t, row));
}

Status tableAddColumn(ScriptTable &table, const string &name)
{
  Elements *t = table.edit();
  if (!t) {
    return gone();
  }
  if (t->column(view(name))) {
    return error(StatusCode::AlreadyExists,
                 std::string("the table has a column '") + name.c_str() + "' already");
  }
  Elements::Column c;
  c.name = name;
  for (int i = 0; i < t->size(); i++) {
    c.values.append(GraphValue());
  }
  t->columns.append(std::move(c));
  return Status();
}

Status tableAddRow(ScriptTable &table, int64_t key)
{
  Elements *t = table.edit();
  if (!t) {
    return gone();
  }
  t->keys.append(key);
  for (Elements::Column &c : t->columns) {
    c.values.append(GraphValue());
  }
  return Status();
}

Status tableSet(ScriptTable &table, const string &column, int32_t row, const PropValue &value)
{
  Result<GraphValue> v = GraphValue::fromProp(value);
  if (!v.ok()) {
    return v.status();
  }
  Elements *t = table.edit();
  if (!t) {
    return gone();
  }
  if (row < 0 || row >= t->size()) {
    return noRow(*t, row);
  }
  for (Elements::Column &c : t->columns) {
    if (view(c.name) == view(column)) {
      c.values[row] = std::move(v).value();
      return Status();
    }
  }
  return noColumn(column);
}

ValueResult tableGet(const ScriptTable &table, const string &column, int32_t row)
{
  const Elements *t = table.get();
  if (!t) {
    return failed<ValueResult>(gone());
  }
  const Elements::Column *c = t->column(view(column));
  if (!c) {
    return failed<ValueResult>(noColumn(column));
  }
  if (row < 0 || row >= t->size()) {
    return failed<ValueResult>(noRow(*t, row));
  }
  if (c->values[row].type() == GraphValue::Type::Payload) {
    return failed<ValueResult>(error(StatusCode::Unsupported,
                                     std::string("column '") + column.c_str() +
                                         "' has payloads, which scripts can't read"));
  }
  return ok<ValueResult>(c->values[row].toProp());
}

const lbt::Struct<ScriptTable> *ScriptTable::defineBindings()
{
  static lbt::Struct<ScriptTable> *st = nullptr;
  if (!st) {
    litestl::alloc::PermanentGuard guard;
    st = new lbt::Struct<ScriptTable>("coredcc::ScriptTable", sizeof(ScriptTable));
    BIND_STRUCT_COPY_CONSTRUCTOR(st);
  }
  return st;
}

CDCC_RESULT_BINDINGS(TableResult)

} // namespace coredcc::script
