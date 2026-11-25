#pragma once

/** \file
 * Tables (graph/foreach.h) for scripts: what `graph.foreach` takes and gives.
 */

#include "coredcc/bind/facades.h"
#include "coredcc/graph/foreach.h"
#include "coredcc/graph/payload.h"

namespace coredcc::script {

/** A table. Copies of it share the rows until one of them is edited, as do sockets that
 * were set to it. */
class ScriptTable {
public:
  ScriptTable() = default;
  explicit ScriptTable(PayloadRef ref) : ref_(std::move(ref))
  {
  }
  /** Null when blocks that the rows are in are gone. */
  const Elements *get() const;
  /** For writing; null as for `get`, and when the rows are a block's. */
  Elements *edit();
  /** Ok if it can be a socket's value: keys are there once, and all rows have a value in
   * all columns. */
  Status check() const;
  /** Null for a table nothing was put into. */
  const PayloadRef &ref() const
  {
    return ref_;
  }
  static const litestl::binding::types::Struct<ScriptTable> *defineBindings();

private:
  PayloadRef ref_;
};

struct TableResult {
  Status status;
  ScriptTable value;
  static const litestl::binding::types::Struct<TableResult> *defineBindings();
};

/** An empty table. */
TableResult newTable();
int32_t tableRows(const ScriptTable &table);
int32_t tableColumns(const ScriptTable &table);
StringResult tableColumnName(const ScriptTable &table, int32_t column);
/** The key of a row, which identifies an element when the table changes. */
IntResult tableKey(const ScriptTable &table, int32_t row);
/** AlreadyExists for a name that is taken. Rows that are there have no value in it. */
Status tableAddColumn(ScriptTable &table, const string &name);
/** A row at the end, without values. A key that is there twice is found by `check`. */
Status tableAddRow(ScriptTable &table, int64_t key);
/** Not a Pointer, Enum or Flags. */
Status
tableSet(ScriptTable &table, const string &column, int32_t row, const PropValue &value);
/** NotFound for a column or row that isn't there; Unsupported for a payload. */
ValueResult tableGet(const ScriptTable &table, const string &column, int32_t row);

} // namespace coredcc::script
