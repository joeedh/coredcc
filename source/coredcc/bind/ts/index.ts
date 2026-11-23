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
import {
  BindingManager,
  createWasmHelpers,
  getBindingBackend,
  setBindingBackend,
  WasmBindingBackend,
  type IBindingBackend,
} from '@litestl/typescript-runtime'
import type {CommandDef} from 'coredcc-gen/coredcc/CommandDef'
import {CommandFlags} from 'coredcc-gen/coredcc/CommandFlags'
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PropertyBag} from 'coredcc-gen/coredcc/PropertyBag'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import {PropValueType} from 'coredcc-gen/coredcc/PropValueType'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

export {CommandFlags, StatusCode}
export type {Context}

/** A value as scripts read it. Vectors, quaternions and Mat4 are arrays; an Int past
 * 2^53 is a bigint; a pointer (or any value without a JS form) stays an owned
 * PropValue; None is `undefined`. */
export type Value = boolean | number | bigint | string | number[] | PropValue | undefined

/** A Float, even when the number is whole: `inputs: {factor: float(1)}`. */
export class FloatValue {
  constructor(readonly value: number) {}
}
export const float = (value: number) => new FloatValue(value)

/** A value scripts can write. A whole number is an Int (use `float()` for a Float);
 * Mat4 can't be written yet. */
export type Writable = boolean | number | bigint | string | ArrayLike<number> | FloatValue | PropValue

/**
 * A failed coredcc call. `message` reads "Code: message (did you mean: ...)"; `detail`
 * is the Status's own message, which a script command's slot passes back on.
 */
export class ScriptError extends Error {
  readonly code: StatusCode
  readonly detail: string
  constructor(code: StatusCode, detail: string, message = detail) {
    super(message)
    this.name = 'ScriptError'
    this.code = code
    this.detail = detail
  }
  static from(s: Status): ScriptError {
    return new ScriptError(s.code(), s.message(), cd.statusText(s))
  }
}

interface Disposable {
  [Symbol.dispose](): void
}
interface ResultOf<T> extends Disposable {
  status: Status
  value: T
}

/** Throws a ScriptError for a failed Status, and disposes of it either way. */
function check(s: Status) {
  try {
    if (!s.ok()) throw ScriptError.from(s)
  } finally {
    s[Symbol.dispose]()
  }
}

/** `read(r.value)` for an ok result; a ScriptError otherwise. Disposes of `r`. */
function take<T, J>(r: ResultOf<T>, read: (v: T) => J): J {
  try {
    if (!r.status.ok()) throw ScriptError.from(r.status)
    return read(r.value)
  } finally {
    r[Symbol.dispose]()
  }
}

const isFatal = (e: unknown) =>
  e instanceof WebAssembly.RuntimeError || (e as {name?: unknown} | null)?.name === 'ExitStatus'

/* ---- loading ---- */

let loaded: {module: object; backend: IBindingBackend} | undefined

/**
 * Makes `module` (an instantiated Emscripten module) the active binding backend,
 * through the BindingManager its `getManager` export returns.
 *
 * One coredcc module per JS realm: bound functions resolve against the active backend,
 * and a wrapper from one module would read another's memory. Loading the same module
 * again returns its backend; another module throws unless `replace` is set, which
 * invalidates everything made through the first.
 */
export function load(module: object, getManager = 'CDCC_GetManager', opts: {replace?: boolean} = {}): IBindingBackend {
  if (loaded?.module === module) return loaded.backend
  if (loaded && !opts.replace) {
    throw new Error('load: another coredcc module is loaded (pass {replace: true} to switch)')
  }
  const m = module as Record<string, unknown>
  if (!(m.HEAPU8 instanceof Uint8Array) || typeof m.wasmMemory !== 'object') {
    throw new TypeError('load: expected an instantiated Emscripten module (HEAPU8, wasmMemory)')
  }
  if (typeof m['_' + getManager] !== 'function' && typeof m[getManager] !== 'function') {
    throw new TypeError(`load: the module exports no ${getManager}`)
  }
  const wasm = createWasmHelpers(module as never, undefined) as unknown as Record<string, () => number>
  const manager = new BindingManager(wasm as never, wasm[getManager]())
  manager.load()
  const backend = new WasmBindingBackend(manager)
  setBindingBackend(backend)
  /* A slot returning nothing returns an ok Status. */
  const ok = backend.construct('coredcc::Status') as Status
  backend.setForeignConverter('coredcc::Status', {ret: (v) => (v === undefined ? ok : v)})
  loaded = {module, backend}
  return backend
}

/* ---- sessions ---- */

/* Script command slots in progress: a document can't close under one. */
let slotDepth = 0

/**
 * A document's context, tied to the host object that owns it. The context keeps the
 * host alive, so GC can't free the document while the context is reachable, and
 * disposing of the session invalidates the context: later calls throw.
 */
export class Session implements Disposable {
  readonly ctx: Context
  private host: Disposable | undefined
  constructor(host: Disposable, ctx: Context) {
    this.host = host
    this.ctx = ctx
    Object.defineProperty(ctx, '__host', {value: host, enumerable: false})
  }
  get disposed() {
    return this.host === undefined
  }
  /** Closes the document. Not from inside a script command's slot. */
  [Symbol.dispose]() {
    if (!this.host) return
    if (slotDepth > 0) throw new Error("Session: can't close the document from inside a command")
    ;(this.ctx as unknown as {ptr: number}).ptr = 0
    const host = this.host
    this.host = undefined
    host[Symbol.dispose]()
  }
}

/** Throws unless `ctx` still addresses a live context. */
function checkCtx(ctx: Context) {
  if (!(ctx as unknown as {ptr?: number}).ptr) throw new Error('the context was disposed')
}

/* ---- values ---- */

function copyValue(v: PropValue): PropValue {
  return getBindingBackend().constructWith('coredcc::PropValue', 'copy', v) as PropValue
}

const toNumber = (n: bigint): number | bigint => (n >= -(2n ** 53n) && n <= 2n ** 53n ? Number(n) : n)

/** A JS form of `v`, copied out of it. */
export function toJs(v: PropValue): Value {
  const components = () => {
    const out: number[] = []
    for (let i = 0; i < v.componentCount(); i++) out.push(take(cd.readComponent(v, i), (x) => x))
    return out
  }
  switch (v.type()) {
    case PropValueType.None:
      return undefined
    case PropValueType.Bool:
      return take(cd.readBool(v), (x) => x)
    case PropValueType.Int:
      return toNumber(take(cd.readInt(v), (x) => BigInt(x)))
    case PropValueType.Float:
      return take(cd.readFloat(v), (x) => x)
    case PropValueType.String:
      return take(cd.readString(v), (x) => x)
    case PropValueType.Enum:
      return Number(take(cd.readEnum(v), (x) => x))
    case PropValueType.Flags:
      return toNumber(BigInt.asUintN(64, BigInt(take(cd.readFlags(v), (x) => x))))
    case PropValueType.Vec2:
    case PropValueType.Vec3:
    case PropValueType.Vec4:
    case PropValueType.Quat:
    case PropValueType.Mat4:
      return components()
    default:
      return copyValue(v)
  }
}

const isBoundValue = (x: unknown): x is PropValue => getBindingBackend().typeName(x) === 'coredcc::PropValue'
const isArrayLike = (x: unknown): x is ArrayLike<number> =>
  Array.isArray(x) || (ArrayBuffer.isView(x) && !(x instanceof DataView))

const INT64_MIN = -(2n ** 63n)
const INT64_MAX = 2n ** 63n - 1n

/** An owned PropValue for `x`. A whole number is an Int, unless it is past 2^53 (then
 * a Float, which C++ range-checks); `float()` makes a Float. Bound PropValues are
 * copied. */
export function fromJs(x: Writable): PropValue {
  if (typeof x === 'boolean') return cd.makeBool(x)
  if (typeof x === 'bigint') {
    if (x < INT64_MIN || x > INT64_MAX) throw new RangeError(`fromJs: ${x} is outside int64`)
    return cd.makeInt(x)
  }
  if (typeof x === 'number') {
    return Number.isSafeInteger(x) ? cd.makeInt(BigInt(x)) : cd.makeFloat(x)
  }
  if (typeof x === 'string') return cd.makeString(x)
  if (x instanceof FloatValue) return cd.makeFloat(x.value)
  if (isArrayLike(x)) {
    const c = Array.from(x, Number)
    if (c.length === 2) return cd.makeVec2(c[0], c[1])
    if (c.length === 3) return cd.makeVec3(c[0], c[1], c[2])
    if (c.length === 4) return cd.makeVec4(c[0], c[1], c[2], c[3])
    throw new TypeError(`fromJs: a vector has 2-4 components, not ${c.length} (Mat4 can't be written yet)`)
  }
  if (isBoundValue(x)) return copyValue(x)
  throw new TypeError(`fromJs: can't make a value of ${String(x)}`)
}

function withValue<R>(x: Writable, fn: (v: PropValue) => R): R {
  const v = fromJs(x)
  try {
    return fn(v)
  } finally {
    v[Symbol.dispose]()
  }
}

function bagToJs(bag: PropertyBag): Record<string, Value> {
  const out: Record<string, Value> = {}
  const n = cd.bagSize(bag)
  for (let i = 0; i < n; i++) {
    const name = take(cd.bagName(bag, i), (s) => s)
    /* defineProperty: an output named __proto__ is data, not the prototype. */
    const value = take(cd.bagGet(bag, name), toJs)
    Object.defineProperty(out, name, {value, enumerable: true, writable: true, configurable: true})
  }
  return out
}

/* ---- datapaths and commands ---- */

/** Throws (NotFound, with suggestions in the message) unless `path` resolves. */
export function resolve(ctx: Context, path: string) {
  check(cd.resolvePath(ctx, path))
}
export function get(ctx: Context, path: string): Value {
  return take(cd.getValue(ctx, path), toJs)
}
/** An undoable set; from a command's exec, a sub-command. */
export function set(ctx: Context, path: string, value: Writable) {
  withValue(value, (v) => check(cd.setValue(ctx, path, v)))
}
/** The raw, unrecorded write, for a command's exec, undo and redo. */
export function write(ctx: Context, path: string, value: Writable) {
  withValue(value, (v) => check(cd.writeValue(ctx, path, v)))
}
/** Runs `mesh.extrude(distance=2)`, returning its outputs. */
export function invoke(ctx: Context, invocation: string): Record<string, Value> {
  return take(cd.invoke(ctx, invocation), bagToJs)
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
  return take(cd.inputGet(ctx, name), toJs)
}
/** Sets the running command's output `name` (inside a slot). */
export function output(ctx: Context, name: string, value: Writable) {
  withValue(value, (v) => check(cd.outputSet(ctx, name, v)))
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
    throw new TypeError(`${slot} returned a Promise: command slots are synchronous`)
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
    slotDepth++
    try {
      return fn()
    } finally {
      slotDepth--
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
      withValue(v, (pv) => check(cd.defInput(def, name, pv)))
    }
    for (const [name, v] of Object.entries(spec.outputs ?? {})) {
      withValue(v, (pv) => check(cd.defOutput(def, name, pv)))
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
