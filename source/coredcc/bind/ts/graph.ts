/**
 * Graphs: node types with TS kernels, editing, and evaluating.
 */
import {getBindingBackend} from '@litestl/typescript-runtime'
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {NodeExecContext as RawNode} from 'coredcc-gen/coredcc/NodeExecContext'
import type {NodeTypeDef} from 'coredcc-gen/coredcc/NodeTypeDef'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import type {ScriptGraph} from 'coredcc-gen/coredcc/ScriptGraph'
import type {ScriptTable} from 'coredcc-gen/coredcc/ScriptTable'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

import {
  check,
  checkCtx,
  copyValue,
  ScriptError,
  scriptSlots,
  statusOf,
  sync,
  take,
  toJs,
  toNumber,
  withValue,
  type Disposable,
  type ResultOf,
  type Value,
  type Writable,
} from './core.ts'

/* ---- tables ---- */

/** The key of a row: an int64. Read, it is a bigint only past 2^53. */
export type TableKey = number | bigint

/** A table, as `graph.foreach` takes and gives them: rows with unique keys, by which elements
 * are found again when the table changes. A copy: changing it changes no graph. Columns have a
 * value per key. */
export class Table {
  constructor(
    readonly keys: TableKey[] = [],
    readonly columns: Record<string, Value[]> = {}
  ) {}
}

function tableToJs(t: ScriptTable): Table {
  const rows = cd.tableRowCount(t)
  const out = new Table()
  for (let i = 0; i < rows; i++) out.keys.push(take(cd.tableKey(t, i), (k) => toNumber(BigInt(k))))
  const columns = cd.tableColumnCount(t)
  for (let c = 0; c < columns; c++) {
    const name = take(cd.tableColumnName(t, c), (s) => s)
    const value = out.keys.map((_, i) => take(cd.tableGet(t, name, i), (v) => toJs(undefined, v)))
    Object.defineProperty(out.columns, name, {value, enumerable: true, writable: true, configurable: true})
  }
  return out
}

function keyOf(key: TableKey): bigint {
  if (typeof key === 'number' && !(Number.isInteger(key) && Math.abs(key) <= 2 ** 53)) {
    throw new TypeError(`a table's key is an integer, and a bigint past 2^53, not ${key}`)
  }
  const k = BigInt(key)
  if (BigInt.asIntN(64, k) !== k) throw new RangeError(`a table's key ${k} is outside int64`)
  return k
}

/** `fn` with the bound form of a table. Its cells are written as `set` writes: floats with
 * `float()`, if they are to be Floats for sure. */
function withTable<R>(table: Table, fn: (t: ScriptTable) => R): R {
  const t = take(
    cd.newTable(),
    (v) => getBindingBackend().constructWith('coredcc::ScriptTable', 'copy', v) as ScriptTable
  )
  try {
    for (const key of table.keys) check(cd.tableAddRow(t, keyOf(key)))
    for (const [name, values] of Object.entries(table.columns)) {
      if (values.length !== table.keys.length) {
        throw new TypeError(`a table's column '${name}' has ${values.length} values for ${table.keys.length} keys`)
      }
      check(cd.tableAddColumn(t, name))
      values.forEach((value, i) => {
        withValue(undefined, value as Writable, (v) => check(cd.tableSet(t, name, i, v)))
      })
    }
    return fn(t)
  } finally {
    t[Symbol.dispose]()
  }
}

/* ---- node types ---- */

/**
 * A node being evaluated, valid during its `evaluate`. A kernel computes its outputs from its
 * inputs and params alone; inside it, everything that takes a Context fails (Busy).
 */
export class NodeExecContext {
  private node: RawNode | undefined
  constructor(node: RawNode) {
    this.node = node
  }
  private get raw(): RawNode {
    if (!this.node) throw new Error('the node was already evaluated')
    return this.node
  }
  /** Not for payload inputs: a table has `table`, and others can only be passed on. */
  input(name: string): Value {
    return take(cd.nodeInput(this.raw, name), (v) => toJs(undefined, v))
  }
  param(name: string): Value {
    return take(cd.nodeParam(this.raw, name), (v) => toJs(undefined, v))
  }
  inputTable(name: string): Table {
    return take(cd.nodeInputTable(this.raw, name), tableToJs)
  }
  isLinked(name: string): boolean {
    return take(cd.nodeIsLinked(this.raw, name), (x) => x)
  }
  /** Makes the node run again when the block that a pointer param points to changes; in one of
   * `aspects` (C++'s ChangeAspects), if given. The block's id, or "" for none. */
  useBlock(param: string, aspects = 0): string {
    return take(cd.nodeUseBlock(this.raw, param, aspects), (x) => x)
  }
  /** Whether the evaluation should end (GraphInstance's `timeLimit`). A kernel that takes long
   * asks now and then, and calls `cancel`. */
  get cancelled(): boolean {
    return cd.nodeCancelled(this.raw)
  }
  /** Ends the kernel. The node runs again when it is needed, as it isn't cached. */
  cancel(): never {
    throw new ScriptError(StatusCode.Cancelled, 'cancelled')
  }
  /** Says how far the kernel is, 0 to 1. */
  reportProgress(fraction: number, text = '') {
    check(cd.nodeProgress(this.raw, fraction, text))
  }
  /** Converted to the output's type, as `set` converts to a member's. */
  setOutput(name: string, value: Writable | Table) {
    const raw = this.raw
    if (value instanceof Table) withTable(value, (t) => check(cd.nodeSetOutputTable(raw, name, t)))
    else withValue(undefined, value, (v) => check(cd.nodeSetOutput(raw, name, v)))
  }
  /** Sets an output to an input of the same type. */
  pass(input: string, output: string) {
    check(cd.nodePass(this.raw, input, output))
  }
  /** Called when the evaluation ends. */
  close() {
    this.node = undefined
  }
}

/** A payload input of a node type: `inputs: {mesh: payload('my.Mesh')}`, or
 * `payload('elements')` for a table. */
export class PayloadInput {
  constructor(readonly socketType: string) {}
}
export const payload = (socketType: string) => new PayloadInput(socketType)

/** A param with items: one of them for `enumParam({A: 0, B: 1}, 'A')`, any of them (single bits)
 * for `flagsParam({X: 1, Y: 2}, ['X'])`. Values past 2^53 are bigints. */
export class ItemParam {
  constructor(
    readonly items: Record<string, number | bigint>,
    readonly value: bigint,
    readonly flags: boolean
  ) {}
}
const itemOf = (items: Record<string, number | bigint>, key: string) => {
  if (!Object.hasOwn(items, key)) throw new TypeError(`there is no item '${key}'`)
  return BigInt(items[key])
}
export const enumParam = (items: Record<string, number | bigint>, value: string) =>
  new ItemParam(items, itemOf(items, value), false)
export const flagsParam = (items: Record<string, number | bigint>, value: string[] = []) =>
  new ItemParam(items, value.reduce((bits, key) => bits | itemOf(items, key), 0n), true)

/** A pointer to blocks of an api struct. A weak one doesn't keep them. */
export class PointerParam {
  constructor(
    readonly target: string,
    readonly weak: boolean
  ) {}
}
export const pointerParam = (target: string, opts: {weak?: boolean} = {}) =>
  new PointerParam(target, opts.weak ?? false)

export interface NodeTypeSpec {
  /** Dotted, e.g. "my.scale". coredcc reserves `core.`, `graph.`, `scene.`, `math.`, `vec.`, `mat4.`, `value.` and `elements.`. */
  id: string
  uiName?: string
  /** Inputs by name, typed by their defaults (Bool, Int, Float, String, Vec2-4, Quat, Mat4). A
   * whole number declares an Int: use `float(1)` for a Float. */
  inputs?: Record<string, Writable | PayloadInput>
  params?: Record<string, Writable | ItemParam | PointerParam>
  /** Socket types by name: `{value: 'float'}`. */
  outputs: Record<string, string>
  /** What inputs and outputs are shown as, where it isn't made of their names. */
  inputNames?: Record<string, string>
  outputNames?: Record<string, string>
  /** The ranges of numbers, by the names of params and inputs: `{amount: [0, 1]}`. */
  ranges?: Record<string, readonly [number, number]>
  /** Programs look for nodes of the type, to do what they say (NodeTypeFlags::OutputAdapter). */
  outputAdapter?: boolean
  /** Sets every output. Synchronous. A throw fails the node (Internal, or a ScriptError's
   * code). One function for all nodes of the type: it keeps no state. */
  evaluate(node: NodeExecContext): void
}

function fill(def: NodeTypeDef, spec: NodeTypeSpec) {
  if (spec.uiName !== undefined) check(cd.nodeTypeDefUiName(def, spec.uiName))
  if (spec.outputAdapter) check(cd.nodeTypeDefOutputAdapter(def))
  for (const [name, v] of Object.entries(spec.inputs ?? {})) {
    if (v instanceof PayloadInput) check(cd.nodeTypeDefPayloadInput(def, name, v.socketType))
    else withValue(undefined, v, (pv) => check(cd.nodeTypeDefInput(def, name, pv)))
  }
  for (const [name, v] of Object.entries(spec.params ?? {})) {
    if (v instanceof PointerParam) check(cd.nodeTypeDefPointerParam(def, name, v.target, v.weak))
    else if (v instanceof ItemParam) {
      check(cd.nodeTypeDefItemParam(def, name, v.flags, v.value))
      for (const [key, value] of Object.entries(v.items)) {
        check(cd.nodeTypeDefItem(def, name, key, BigInt(value), ''))
      }
    } else withValue(undefined, v, (pv) => check(cd.nodeTypeDefParam(def, name, pv)))
  }
  for (const [name, type] of Object.entries(spec.outputs)) check(cd.nodeTypeDefOutput(def, name, type))
  for (const [name, ui] of Object.entries(spec.inputNames ?? {})) check(cd.nodeTypeDefSocketName(def, false, name, ui))
  for (const [name, ui] of Object.entries(spec.outputNames ?? {})) check(cd.nodeTypeDefSocketName(def, true, name, ui))
  for (const [name, [min, max]] of Object.entries(spec.ranges ?? {})) check(cd.nodeTypeDefRange(def, name, min, max))
}

type Register = (ctx: Context, def: NodeTypeDef, kernel: number, slots: string) => Status

function register(what: string, ctx: Context, spec: NodeTypeSpec, enter: Register) {
  checkCtx(ctx)
  if (typeof spec.evaluate !== 'function') {
    throw new TypeError(`${what}: ${spec.id}: evaluate must be a function`)
  }
  const backend = getBindingBackend()
  const def = cd.newNodeTypeDef(spec.id)
  if (!def) throw new Error(`${what}: no def for ${spec.id}`)
  try {
    fill(def, spec)
    const kernel = {
      evaluate(raw: RawNode): Status | undefined {
        const node = new NodeExecContext(raw)
        scriptSlots.callDepth++
        try {
          sync('evaluate', spec.evaluate(node))
          return undefined
        } catch (e) {
          return statusOf(StatusCode.Internal, e)
        } finally {
          scriptSlots.callDepth--
          node.close()
        }
      },
    }
    check(enter(ctx, def, backend.foreignHandle(kernel), 'evaluate'))
  } finally {
    def[Symbol.dispose]()
  }
}

/** Registers a node type, for as long as the document is open. */
export function defineNodeType(ctx: Context, spec: NodeTypeSpec) {
  register('defineNodeType', ctx, spec, cd.registerForeignNodeType)
}

/** Replaces the kernel of a node type that a script defined, for code that is reloaded. Its
 * nodes run again when they are needed. The rest of `spec` has to say what it did; texts, ranges
 * and defaults stay as they were. */
export function replaceNodeType(ctx: Context, spec: NodeTypeSpec) {
  register('replaceNodeType', ctx, spec, cd.replaceForeignNodeType)
}

/* ---- editing ---- */

/** Makes a graph block; a pointer to it, for a pointer member. Undoable, as are the other
 * edits, which take a path to a graph or to a pointer to one. */
export function newGraph(ctx: Context, name: string): PropValue {
  return take(cd.newGraph(ctx, name), copyValue)
}
/** The new node's id. */
export function addGraphNode(ctx: Context, path: string, type: string, x = 0, y = 0): number {
  return take(cd.graphAddNode(ctx, path, type, x, y), Number)
}
export function removeGraphNode(ctx: Context, path: string, node: number) {
  check(cd.graphRemoveNode(ctx, path, BigInt(node)))
}
/** The new link's id. Sockets are named by key: an input's or output's name, or `#<id>` for
 * one of a graph's interface (on group, input and output nodes). */
export function addGraphLink(
  ctx: Context,
  path: string,
  from: number,
  fromSocket: string,
  to: number,
  toSocket: string
): number {
  return take(cd.graphLink(ctx, path, BigInt(from), fromSocket, BigInt(to), toSocket), Number)
}
export function removeGraphLink(ctx: Context, path: string, link: number) {
  check(cd.graphUnlink(ctx, path, BigInt(link)))
}
/** The new interface socket's id. */
export function addGraphInterface(
  ctx: Context,
  path: string,
  side: 'input' | 'output',
  name: string,
  type: string
): number {
  return take(cd.graphAddInterface(ctx, path, side === 'output', name, type), Number)
}

/** An id that nothing in the graph has, for pasting. */
export function graphNewId(ctx: Context, path: string): number {
  return take(cd.graphNewId(ctx, path), Number)
}
/** A link that doesn't work, and why. `interior`: it leads to a node inside a group. */
export interface LinkDiagnostic {
  link: number
  code: string
  message: string
  interior: boolean
}
export function graphDiagnostics(ctx: Context, path: string): LinkDiagnostic[] {
  return take(cd.graphDiagnostics(ctx, path), (s) => JSON.parse(s) as LinkDiagnostic[])
}

/* ---- evaluating ---- */

/** What is known of a node (bind/graph_instance_facades.h). `node` in an error is a path. */
export interface NodeReport {
  state: 'NotEvaluated' | 'Ok' | 'Failed' | 'InputFailed'
  current: boolean
  runs: number
  seconds: number
  totalSeconds: number
  elements: number
  cachedElements: number
  memory: {frames: number; kernels: number; elements: number; bytes: number; payloadBytes: number}
  error?: {code: string; message: string; node: string; socket?: string}
}

/** A watched socket: what the last `update` of its instance gave. */
export class Watch implements Disposable {
  constructor(
    private readonly graph: () => ScriptGraph,
    /** As `update` lists it. */
    readonly id: number
  ) {}
  private read<T, J>(fn: (graph: ScriptGraph, id: bigint) => ResultOf<T>, read: (v: T) => J): J {
    return take(fn(this.graph(), BigInt(this.id)), read)
  }
  /** Throws the socket's error when it has none. */
  get value(): Value {
    return this.read(cd.graphWatchValue, (v) => toJs(undefined, v))
  }
  get table(): Table {
    return this.read(cd.graphWatchTable, tableToJs)
  }
  /** The last value, for display while there is an error. */
  get lastGood(): Value {
    return this.read(cd.graphWatchLastGood, (v) => toJs(undefined, v))
  }
  /** Moves with the value and the error; 0 before an update. */
  get changedAt(): number {
    return this.read(cd.graphWatchChangedAt, Number)
  }
  /** False if an update might give something new. Cheap. */
  get isCurrent(): boolean {
    return this.read(cd.graphWatchIsCurrent, (x) => x)
  }
  /** "" when there is none. */
  get errorText(): string {
    return cd.graphWatchErrorText(this.graph(), BigInt(this.id))
  }
  /** Stops watching. */
  [Symbol.dispose]() {
    check(cd.graphUnwatch(this.graph(), BigInt(this.id)))
  }
}

/** An evaluated graph. Nodes are named by path: their id, or `12/34` for one in a group and
 * `12/34[7]` for one in element 7 of a foreach. Closed by `using`, and with its document. */
export class GraphInstance implements Disposable {
  private raw: ScriptGraph | undefined
  constructor(ctx: Context, path: string) {
    checkCtx(ctx)
    this.raw = take(
      cd.newGraphInstance(ctx, path),
      (v) => getBindingBackend().constructWith('coredcc::ScriptGraph', 'copy', v) as ScriptGraph
    )
  }
  private get graph(): ScriptGraph {
    if (!this.raw) throw new Error('the graph instance was disposed')
    return this.raw
  }
  /** Sets an interface input by name; `undefined` goes back to its default. */
  setInput(name: string, value: Writable | Table | undefined) {
    const graph = this.graph
    if (value === undefined) check(cd.graphClearInput(graph, name))
    else if (value instanceof Table) withTable(value, (t) => check(cd.graphSetInputTable(graph, name, t)))
    else withValue(undefined, value, (v) => check(cd.graphSetInput(graph, name, v)))
  }
  /** An interface output's value, by name. */
  evaluate(output: string): Value {
    return take(cd.graphEvaluate(this.graph, output), (v) => toJs(undefined, v))
  }
  /** One that is a table. */
  evaluateTable(output: string): Table {
    return take(cd.graphEvaluateTable(this.graph, output), tableToJs)
  }
  /** Watches an interface output, or a socket of a node. */
  watch(socket: string, node: string | number = ''): Watch {
    return new Watch(() => this.graph, take(cd.graphWatch(this.graph, String(node), socket), Number))
  }
  /** Evaluates what is watched; the ids of the watches whose value or error moved. */
  update(): number[] {
    return take(cd.graphUpdate(this.graph), (s) => JSON.parse(s) as number[])
  }
  /** The value a socket had when it was last computed, without evaluating. `undefined` if it
   * wasn't, or failed. */
  peek(node: string | number, socket: string, side: 'input' | 'output' = 'output'): Value {
    return take(cd.graphPeek(this.graph, String(node), socket, side === 'input'), (v) => toJs(undefined, v))
  }
  peekTable(node: string | number, socket: string, side: 'input' | 'output' = 'output'): Table {
    return take(cd.graphPeekTable(this.graph, String(node), socket, side === 'input'), tableToJs)
  }
  report(node: string | number): NodeReport {
    return take(cd.graphReport(this.graph, String(node)), (s) => JSON.parse(s) as NodeReport)
  }
  /** Makes failed kernels run again. */
  retry() {
    check(cd.graphRetry(this.graph))
  }
  /** Drops everything evaluated. */
  clear() {
    check(cd.graphClear(this.graph))
  }
  /** Drops what a node keeps; it is computed again when something needs it. */
  drop(node: string | number) {
    check(cd.graphDrop(this.graph, String(node)))
  }
  /** Evaluations that take longer are Cancelled; the next one goes on. 0 for no limit. */
  set timeLimit(milliseconds: number) {
    check(cd.graphSetTimeLimit(this.graph, milliseconds))
  }
  /** Why `output` had no value when last evaluated; "" if it had one. */
  errorText(output: string): string {
    return cd.graphErrorText(this.graph, output)
  }
  /** An evaluation in progress (when a kernel disposes of its own) is completed. */
  [Symbol.dispose]() {
    const raw = this.raw
    this.raw = undefined
    raw?.[Symbol.dispose]()
  }
}
