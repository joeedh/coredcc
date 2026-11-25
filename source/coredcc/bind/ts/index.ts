/**
 * coredcc's TS layer over the generated bindings (P1f step 5 in
 * docs/plans/2026-11-20-p1-application-spine.md): plain JS values in and out, errors
 * as exceptions, and script commands as ordinary classes.
 *
 * Imports the generated TS as `coredcc-gen/*` and litestl's runtime as
 * `@litestl/typescript-runtime`; the host maps both (tests/wasm/run.mjs).
 *
 * Rules the generated shapes impose, handled here so callers never see them:
 * - Result structs and the PropValues inside them are views that don't keep the result
 *   alive, so every result is unwrapped into JS values at once, then disposed.
 * - Facades that change the document are limited per command slot (facades.h).
 * - int64 is `bigint` in the generated types; this layer takes and returns numbers,
 *   and a bigint only for an Int past 2^53.
 *
 * Errors: a failed coredcc call throws a `ScriptError`. Misuse of this layer itself (a
 * value it can't convert, a disposed context, a second module) throws a `TypeError`
 * or `Error`.
 */
import {getBindingBackend} from '@litestl/typescript-runtime'
import type {CommandDef} from 'coredcc-gen/coredcc/CommandDef'
import {CommandFlags} from 'coredcc-gen/coredcc/CommandFlags'
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {NodeExecContext} from 'coredcc-gen/coredcc/NodeExecContext'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import {PropValueType} from 'coredcc-gen/coredcc/PropValueType'
import type {ScriptGraph} from 'coredcc-gen/coredcc/ScriptGraph'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

import {
  bagToJs,
  check,
  checkCtx,
  copyValue,
  isFatal,
  ScriptError,
  scriptSlots,
  take,
  toJs,
  withValue,
  type Disposable,
  type NamedValue,
  type Value,
  type Writable,
} from './core.ts'

export {CommandFlags, StatusCode}
export type {Context}
export {float, FloatValue, fromJs, load, Ref, ScriptError, Session, toJs, type NamedValue, type Value, type Writable} from './core.ts'
export * from './layout.ts'

/* ---- datapaths and commands ---- */

/** What a path is: a PropKind for a member, or what the rest can name. */
export type PathKind =
  | 'Bool' | 'Int' | 'Float' | 'String' | 'Enum' | 'Flags' | 'Vec2' | 'Vec3' | 'Vec4'
  | 'Mat4' | 'Quat' | 'Pointer' | 'Collection' | 'Struct' | 'Component' | 'EnumKey' | 'Length'

/** A property's description, or a path's (bind/meta.h says which fields there are when). An
 * integer past 2^53 is a string. */
export interface PathMeta {
  kind: PathKind
  canonical: string
  struct?: string
  members?: string[]
  component?: number
  of?: PathKind
  key?: string
  readOnly?: boolean
  name?: string
  uiName?: string
  description?: string
  icon?: string
  subtype?: string
  access?: string[]
  ui?: {iconsOnly?: boolean; expand?: boolean; slider?: 'Slider' | 'Number'; multiline?: boolean}
  hardMin?: number
  hardMax?: number
  softMin?: number
  softMax?: number
  step?: number
  precision?: number
  unit?: string
  items?: {key: string; value: number | string; uiName: string; description?: string; icon?: string}[]
  maxLength?: number
  target?: string
  default?: unknown
}

/** What `path` is. Throws (NotFound, with suggestions in the message) unless it resolves. */
export function resolve(ctx: Context, path: string): PathKind {
  return take(cd.resolvePath(ctx, path), (kind) => kind as PathKind)
}
/** What `path` is, in full. */
export function meta(ctx: Context, path: string): PathMeta {
  return JSON.parse(take(cd.pathMeta(ctx, path), (json) => json)) as PathMeta
}
/** With `names`, an enum reads as its key and flags as their keys; a number without an item
 * stays a number. */
export function get(ctx: Context, path: string): Value
export function get(ctx: Context, path: string, opts: {names?: boolean}): NamedValue
export function get(ctx: Context, path: string, opts: {names?: boolean} = {}): NamedValue {
  return take(cd.getValue(ctx, path), (v) => {
    const flags = v.type() === PropValueType.Flags
    if (!opts.names || (!flags && v.type() !== PropValueType.Enum)) return toJs(ctx, v)
    try {
      const keys = take(cd.valueKeys(ctx, path, v), (s) => s)
      return !flags ? keys : keys === '' ? [] : keys.split('|')
    } catch (e) {
      if (e instanceof ScriptError && e.code === StatusCode.NotFound) return toJs(ctx, v)
      throw e
    }
  })
}
/** An undoable set; from a command's exec, a sub-command. Sets of one member with the same
 * `gesture` (a positive integer: one slider drag) are one undo step. */
export function set(ctx: Context, path: string, value: Writable, gesture?: number) {
  if (gesture !== undefined && !(Number.isSafeInteger(gesture) && gesture > 0)) {
    throw new TypeError(`set: a gesture is a positive integer, not ${gesture}`)
  }
  withValue(ctx, value, (v) =>
    check(gesture === undefined ? cd.setValue(ctx, path, v) : cd.setValueGesture(ctx, path, v, BigInt(gesture)))
  )
}
/** The raw, unrecorded write, for a command's exec, undo and redo. */
export function write(ctx: Context, path: string, value: Writable) {
  withValue(ctx, value, (v) => check(cd.writeValue(ctx, path, v)))
}
/** Runs `mesh.extrude(distance=2)`, returning its outputs. */
export function invoke(ctx: Context, invocation: string): Record<string, Value> {
  return take(cd.invoke(ctx, invocation), (bag) => bagToJs(ctx, bag))
}
/** As `invoke`, with a path prefix ("" for none), as a layout's tool has: argument paths start
 * from it, and so do the command's own, also in its undo and redo. */
export function invokeAt(ctx: Context, prefix: string, invocation: string): Record<string, Value> {
  return take(cd.invokeAt(ctx, prefix, invocation), (bag) => bagToJs(ctx, bag))
}
export function undo(ctx: Context) {
  check(cd.undo(ctx))
}
export function redo(ctx: Context) {
  check(cd.redo(ctx))
}
export function canUndo(ctx: Context): boolean {
  return take(cd.canUndo(ctx), (x) => x)
}
export function canRedo(ctx: Context): boolean {
  return take(cd.canRedo(ctx), (x) => x)
}
/** The running command's input `name` (inside a slot). */
export function input(ctx: Context, name: string): Value {
  return take(cd.inputGet(ctx, name), (v) => toJs(ctx, v))
}
/** Sets the running command's output `name` (inside a slot). */
export function output(ctx: Context, name: string, value: Writable) {
  withValue(ctx, value, (v) => check(cd.outputSet(ctx, name, v)))
}

/* ---- script commands ---- */

/**
 * A command written in TS. Slots are methods (not arrow-function fields), and are
 * synchronous: one that returns a Promise fails. `ctx` is valid for the call only.
 * - `canRun`: return nothing (or true) to allow, a string or false to refuse; a throw
 *   refuses too, with its message (FailedPrecondition, or a ScriptError's code).
 * - `exec` captures what `undo` needs. A throw fails the run (Internal, or a
 *   ScriptError's code).
 * - `undo`: a throw drops the command from the history. Its code is reported as
 *   Internal, with the error in the message (undo has no Status to carry one), and so
 *   is a throw from the constructor.
 * - `redo` is optional: without it, redo runs `exec` again.
 * - `memSize` (bytes held for undo) and `discard` are optional; a throw from either is
 *   ignored (reported with console.warn).
 */
export interface ScriptCommand {
  canRun?(ctx: Context): void | boolean | string
  exec(ctx: Context): void
  undo(ctx: Context): void
  redo?(ctx: Context): void
  memSize?(): number
  discard?(): void
}

export interface CommandSpec {
  /** Dotted, e.g. "mesh.extrude". The `core.` namespace is reserved. */
  id: string
  uiName?: string
  description?: string
  icon?: string
  /** CommandFlags bits. */
  flags?: CommandFlags | number
  /** Inputs by name, typed by their defaults (Bool, Int, Float, String, Vec2-4). A
   * whole number declares an Int: use `float(1)` for a Float. */
  inputs?: Record<string, Writable>
  outputs?: Record<string, Writable>
}

/** An error Status for a slot's return, disposed of after C++ copies it. */
function errorStatus(code: StatusCode, message: string): Status {
  const s = cd.statusError(code, message)
  queueMicrotask(() => s[Symbol.dispose]())
  return s
}

function statusOf(code: StatusCode, e: unknown): Status {
  if (isFatal(e)) throw e
  if (e instanceof ScriptError) return errorStatus(e.code, e.detail)
  return errorStatus(code, e instanceof Error ? e.message : String(e))
}

const isThenable = (v: unknown) =>
  (typeof v === 'object' || typeof v === 'function') && v !== null && typeof (v as {then?: unknown}).then === 'function'

/** `r`, unless it is a Promise: slots are synchronous. */
function sync<T>(slot: string, r: T): T {
  if (isThenable(r)) {
    ;(r as unknown as Promise<unknown>).catch(() => {}) /* reported here, not as unhandled */
    throw new TypeError(`${slot} returned a Promise: slots are synchronous`)
  }
  return r
}

/** What C++ calls: the user's command with defaults for the slots it left out. */
class Adapter {
  constructor(
    private cmd: ScriptCommand,
    private id: string,
    hasRedoSlot: boolean
  ) {
    const own = (cmd as {redo?: unknown}).redo
    if (!hasRedoSlot && typeof own === 'function') {
      throw new TypeError(`${id}: redo must be a method, not a field (redo would run exec instead)`)
    }
  }
  private run<T>(fn: () => T): T {
    scriptSlots.depth++
    try {
      return fn()
    } finally {
      scriptSlots.depth--
    }
  }
  canRun(ctx: Context): Status | undefined {
    return this.run(() => {
      try {
        const r = sync('canRun', this.cmd.canRun?.(ctx))
        if (typeof r === 'string') return errorStatus(StatusCode.FailedPrecondition, r)
        if (r === false) return errorStatus(StatusCode.FailedPrecondition, 'refused')
        return undefined
      } catch (e) {
        return statusOf(StatusCode.FailedPrecondition, e)
      }
    })
  }
  exec(ctx: Context): Status | undefined {
    return this.run(() => {
      try {
        sync('exec', this.cmd.exec(ctx))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  undo(ctx: Context) {
    this.run(() => sync('undo', this.cmd.undo(ctx)))
  }
  redo(ctx: Context): Status | undefined {
    return this.run(() => {
      try {
        sync('redo', this.cmd.redo?.(ctx))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  /* A failing memSize or discard would report an unrelated applied run as failed. */
  memSize(): number {
    return this.run(() => {
      try {
        const n = Number(sync('memSize', this.cmd.memSize?.() ?? 0))
        return Number.isFinite(n) && n > 0 ? Math.min(Math.floor(n), Number.MAX_SAFE_INTEGER) : 0
      } catch (e) {
        if (isFatal(e)) throw e
        console.warn(`${this.id}: memSize failed, counted as 0:`, e)
        return 0
      }
    })
  }
  discard() {
    this.run(() => {
      try {
        sync('discard', this.cmd.discard?.())
      } catch (e) {
        if (isFatal(e)) throw e
        console.warn(`${this.id}: discard failed:`, e)
      }
    })
  }
}

/** Registers `Cls` as command `spec.id`. Each run makes a new instance. */
export function registerCommand(ctx: Context, spec: CommandSpec, Cls: new () => ScriptCommand) {
  checkCtx(ctx)
  if (typeof Cls !== 'function' || typeof Cls.prototype !== 'object') {
    throw new TypeError(`registerCommand: ${spec.id}: expected a class, not ${typeof Cls}`)
  }
  const flags = spec.flags ?? 0
  if (!Number.isInteger(flags) || flags < 0 || flags > 0xffffffff) {
    throw new TypeError(`registerCommand: flags must be CommandFlags bits, not ${flags}`)
  }
  const backend = getBindingBackend()
  const def = cd.newCommandDef(spec.id)
  if (!def) throw new Error(`registerCommand: no def for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) check(cd.defUiName(def, spec.uiName))
    if (spec.description !== undefined) check(cd.defDescription(def, spec.description))
    if (spec.icon !== undefined) check(cd.defIcon(def, spec.icon))
    check(cd.defFlags(def, flags))
    for (const [name, v] of Object.entries(spec.inputs ?? {})) {
      withValue(ctx, v, (pv) => check(cd.defInput(def, name, pv)))
    }
    for (const [name, v] of Object.entries(spec.outputs ?? {})) {
      withValue(ctx, v, (pv) => check(cd.defOutput(def, name, pv)))
    }
    /* The adapter supplies every required slot; redo only when the class has one, so
     * a class without it keeps redo-by-exec. */
    const hasRedo = typeof Cls.prototype.redo === 'function'
    const slots = ['canRun', 'exec', 'undo', 'memSize', 'discard']
    if (hasRedo) slots.push('redo')
    const id = spec.id
    const factory = {make: () => backend.foreignHandle(new Adapter(new Cls(), id, hasRedo))}
    /* checkCtx above: a stale ctx refused while packing the arguments would leave the
     * handle's count with nobody to release it. */
    check(cd.registerForeignCommand(ctx, def as CommandDef, backend.foreignHandle(factory), slots.join(' ')))
  } finally {
    def[Symbol.dispose]()
  }
}

/* ---- graphs ---- */

/**
 * A node being evaluated, valid during its `evaluate`. A kernel computes its outputs from its
 * inputs and params alone; inside it, everything that takes a Context fails (Busy).
 */
export class NodeContext {
  private node: NodeExecContext | undefined
  constructor(node: NodeExecContext) {
    this.node = node
  }
  private get raw(): NodeExecContext {
    if (!this.node) throw new Error('the node was already evaluated')
    return this.node
  }
  /** Not for payload inputs, which can only be passed on. */
  input(name: string): Value {
    return take(cd.nodeInput(this.raw, name), (v) => toJs(undefined, v))
  }
  param(name: string): Value {
    return take(cd.nodeParam(this.raw, name), (v) => toJs(undefined, v))
  }
  /** Converted to the output's type, as `set` converts to a member's. */
  output(name: string, value: Writable) {
    const raw = this.raw
    withValue(undefined, value, (v) => check(cd.nodeOutput(raw, name, v)))
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

/** A payload input of a node type: `inputs: {mesh: payload('my.Mesh')}`. */
export class PayloadInput {
  constructor(readonly socketType: string) {}
}
export const payload = (socketType: string) => new PayloadInput(socketType)

export interface NodeTypeSpec {
  /** Dotted, e.g. "my.scale". The `core.` and `graph.` namespaces are reserved. */
  id: string
  uiName?: string
  /** Inputs by name, typed by their defaults (Bool, Int, Float, String, Vec2-4). A whole
   * number declares an Int: use `float(1)` for a Float. */
  inputs?: Record<string, Writable | PayloadInput>
  params?: Record<string, Writable>
  /** Socket types by name: `{value: 'float'}`. */
  outputs: Record<string, string>
  /** Sets every output. Synchronous. A throw fails the node (Internal, or a ScriptError's
   * code). One function for all nodes of the type: it keeps no state. */
  evaluate(node: NodeContext): void
}

/** Registers a node type, for as long as the document is open. */
export function defineNodeType(ctx: Context, spec: NodeTypeSpec) {
  checkCtx(ctx)
  if (typeof spec.evaluate !== 'function') {
    throw new TypeError(`defineNodeType: ${spec.id}: evaluate must be a function`)
  }
  const backend = getBindingBackend()
  const def = cd.newNodeType(spec.id)
  if (!def) throw new Error(`defineNodeType: no def for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) check(cd.nodeTypeUiName(def, spec.uiName))
    for (const [name, v] of Object.entries(spec.inputs ?? {})) {
      if (v instanceof PayloadInput) check(cd.nodeTypePayloadInput(def, name, v.socketType))
      else withValue(undefined, v, (pv) => check(cd.nodeTypeInput(def, name, pv)))
    }
    for (const [name, v] of Object.entries(spec.params ?? {})) {
      withValue(undefined, v, (pv) => check(cd.nodeTypeParam(def, name, pv)))
    }
    for (const [name, type] of Object.entries(spec.outputs)) check(cd.nodeTypeOutput(def, name, type))
    const kernel = {
      evaluate(raw: NodeExecContext): Status | undefined {
        const node = new NodeContext(raw)
        scriptSlots.depth++
        try {
          sync('evaluate', spec.evaluate(node))
          return undefined
        } catch (e) {
          return statusOf(StatusCode.Internal, e)
        } finally {
          scriptSlots.depth--
          node.close()
        }
      },
    }
    check(cd.registerForeignNodeType(ctx, def, backend.foreignHandle(kernel), 'evaluate'))
  } finally {
    def[Symbol.dispose]()
  }
}

/** Makes a graph block; a pointer to it, for a pointer member. Undoable, as are the other
 * edits, which take a path to a graph or to a pointer to one. */
export function graphCreate(ctx: Context, name: string): PropValue {
  return take(cd.graphCreate(ctx, name), copyValue)
}
/** The new node's id. */
export function graphAddNode(ctx: Context, path: string, type: string, x = 0, y = 0): number {
  return take(cd.graphAddNode(ctx, path, type, x, y), Number)
}
export function graphRemoveNode(ctx: Context, path: string, node: number) {
  check(cd.graphRemoveNode(ctx, path, BigInt(node)))
}
/** The new link's id. Sockets are named by key: an input's or output's name, or `#<id>` for
 * one of a graph's interface (on group, input and output nodes). */
export function graphLink(
  ctx: Context,
  path: string,
  from: number,
  fromSocket: string,
  to: number,
  toSocket: string
): number {
  return take(cd.graphLink(ctx, path, BigInt(from), fromSocket, BigInt(to), toSocket), Number)
}
export function graphUnlink(ctx: Context, path: string, link: number) {
  check(cd.graphUnlink(ctx, path, BigInt(link)))
}
/** The new interface socket's id. */
export function graphAddInterface(
  ctx: Context,
  path: string,
  side: 'input' | 'output',
  name: string,
  type: string
): number {
  return take(cd.graphAddInterface(ctx, path, side === 'output', name, type), Number)
}

/** An evaluated graph. Closed by `using`, and with its document. */
export class GraphInstance implements Disposable {
  private raw: ScriptGraph | undefined
  constructor(ctx: Context, path: string) {
    checkCtx(ctx)
    this.raw = take(
      cd.graphInstance(ctx, path),
      (v) => getBindingBackend().constructWith('coredcc::ScriptGraph', 'copy', v) as ScriptGraph
    )
  }
  private get graph(): ScriptGraph {
    if (!this.raw) throw new Error('the graph instance was disposed')
    return this.raw
  }
  /** Sets an interface input by name; `undefined` goes back to its default. */
  setInput(name: string, value: Writable | undefined) {
    const graph = this.graph
    if (value === undefined) check(cd.graphClearInput(graph, name))
    else withValue(undefined, value, (v) => check(cd.graphSetInput(graph, name, v)))
  }
  /** An interface output's value, by name. */
  evaluate(output: string): Value {
    return take(cd.graphEvaluate(this.graph, output), (v) => toJs(undefined, v))
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
