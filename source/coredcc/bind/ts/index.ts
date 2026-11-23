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
 * - int64 is `bigint` in the generated types; this layer takes and returns numbers.
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
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PropertyBag} from 'coredcc-gen/coredcc/PropertyBag'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import {PropValueType} from 'coredcc-gen/coredcc/PropValueType'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

export {StatusCode}
export type {Context}

/** A value as scripts see it. Vectors and quaternions are arrays; a pointer (or any
 * other value without a JS form) stays an owned PropValue. */
export type Value = boolean | number | string | number[] | PropValue | undefined

/**
 * A failed coredcc call. `message` reads "Code: message (did you mean: ...)"; `detail`
 * is the Status's own message, which a script command's slot passes back on.
 */
export class ScriptError extends Error {
  readonly detail: string
  constructor(
    public code: StatusCode,
    detail: string,
    message = detail
  ) {
    super(message)
    this.name = 'ScriptError'
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

/**
 * Makes `module` (an instantiated Emscripten module) the active binding backend, through
 * the BindingManager its `getManager` export returns. Call once per module.
 */
export function load(module: object, getManager = 'CDCC_GetManager'): IBindingBackend {
  const wasm = createWasmHelpers(module as never, undefined) as unknown as Record<string, unknown>
  const get = wasm[getManager]
  if (typeof get !== 'function') throw new Error(`load: the module exports no ${getManager}`)
  const ptr = (get as () => number)()
  const manager = new BindingManager(wasm as never, ptr)
  manager.load()
  const backend = new WasmBindingBackend(manager)
  setBindingBackend(backend)
  /* A slot returning nothing returns an ok Status. */
  const ok = backend.construct('coredcc::Status') as Status
  backend.setForeignConverter('coredcc::Status', {ret: (v) => (v === undefined ? ok : v)})
  return backend
}

/* ---- values ---- */

function copyValue(v: PropValue): PropValue {
  return getBindingBackend().constructWith('coredcc::PropValue', 'copy', v) as PropValue
}

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
      return Number(take(cd.readInt(v), (x) => x))
    case PropValueType.Float:
      return take(cd.readFloat(v), (x) => x)
    case PropValueType.String:
      return take(cd.readString(v), (x) => x)
    case PropValueType.Enum:
      return Number(take(cd.readEnum(v), (x) => x))
    case PropValueType.Flags:
      return Number(take(cd.readFlags(v), (x) => x))
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

/** An owned PropValue for `x`: integers become Int, other numbers Float. Bound
 * PropValues are copied. */
export function fromJs(x: Value | bigint): PropValue {
  if (typeof x === 'boolean') return cd.makeBool(x)
  if (typeof x === 'bigint') return cd.makeInt(x)
  if (typeof x === 'number') return Number.isInteger(x) ? cd.makeInt(BigInt(x)) : cd.makeFloat(x)
  if (typeof x === 'string') return cd.makeString(x)
  if (Array.isArray(x)) {
    if (x.length === 2) return cd.makeVec2(x[0], x[1])
    if (x.length === 3) return cd.makeVec3(x[0], x[1], x[2])
    if (x.length === 4) return cd.makeVec4(x[0], x[1], x[2], x[3])
    throw new TypeError(`fromJs: a vector has 2-4 components, not ${x.length}`)
  }
  if (isBoundValue(x)) return copyValue(x)
  throw new TypeError(`fromJs: can't make a value of ${String(x)}`)
}

function withValue<R>(x: Value | bigint, fn: (v: PropValue) => R): R {
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
    out[name] = take(cd.bagGet(bag, name), toJs)
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
export function set(ctx: Context, path: string, value: Value) {
  withValue(value, (v) => check(cd.setValue(ctx, path, v)))
}
/** The raw, unrecorded write, for a command's exec, undo and redo. */
export function write(ctx: Context, path: string, value: Value) {
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
export function output(ctx: Context, name: string, value: Value) {
  withValue(value, (v) => check(cd.outputSet(ctx, name, v)))
}

/* ---- script commands ---- */

/**
 * A command written in TS. `ctx` is valid for the call only.
 * - `canRun`: return nothing (or true) to allow, a string or false to refuse; a throw
 *   refuses too, with its message.
 * - `exec` captures what `undo` needs. A throw fails the run.
 * - `redo` is optional: without it, redo runs `exec` again.
 * - `memSize` (bytes held for undo) and `discard` are optional.
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
  /** CommandFlags bits (C++ command.h). */
  flags?: number
  /** Inputs by name, typed by their defaults (Bool, Int, Float, String, Vec2-4). */
  inputs?: Record<string, Value>
  outputs?: Record<string, Value>
}

/** A Status for a slot: ok, or `code` with the error's message. */
function statusOf(code: StatusCode, e: unknown): Status {
  if (isFatal(e)) throw e
  if (e instanceof ScriptError) return cd.statusError(e.code, e.detail)
  return cd.statusError(code, e instanceof Error ? e.message : String(e))
}

/** What C++ calls: the user's command with defaults for the slots it left out. */
class Adapter {
  constructor(private cmd: ScriptCommand) {}
  canRun(ctx: Context): Status | undefined {
    try {
      const r = this.cmd.canRun?.(ctx)
      if (typeof r === 'string') return cd.statusError(StatusCode.FailedPrecondition, r)
      if (r === false) return cd.statusError(StatusCode.FailedPrecondition, 'refused')
      return undefined
    } catch (e) {
      return statusOf(StatusCode.FailedPrecondition, e)
    }
  }
  exec(ctx: Context): Status | undefined {
    try {
      this.cmd.exec(ctx)
      return undefined
    } catch (e) {
      return statusOf(StatusCode.Internal, e)
    }
  }
  undo(ctx: Context) {
    this.cmd.undo(ctx)
  }
  redo(ctx: Context): Status | undefined {
    try {
      this.cmd.redo?.(ctx)
      return undefined
    } catch (e) {
      return statusOf(StatusCode.Internal, e)
    }
  }
  /* A failing memSize would report an applied run as failed. */
  memSize(): number {
    try {
      const n = Number(this.cmd.memSize?.() ?? 0)
      return Number.isFinite(n) && n > 0 ? Math.floor(n) : 0
    } catch (e) {
      if (isFatal(e)) throw e
      return 0
    }
  }
  discard() {
    this.cmd.discard?.()
  }
}

/** Registers `Cls` as command `spec.id`. Each run makes a new instance. */
export function registerCommand(ctx: Context, spec: CommandSpec, Cls: new () => ScriptCommand) {
  const backend = getBindingBackend()
  const def = cd.newCommandDef(spec.id) as CommandDef
  try {
    if (spec.uiName !== undefined) check(cd.defUiName(def, spec.uiName))
    if (spec.description !== undefined) check(cd.defDescription(def, spec.description))
    if (spec.icon !== undefined) check(cd.defIcon(def, spec.icon))
    if (spec.flags !== undefined) check(cd.defFlags(def, spec.flags))
    for (const [name, v] of Object.entries(spec.inputs ?? {})) {
      withValue(v, (pv) => check(cd.defInput(def, name, pv)))
    }
    for (const [name, v] of Object.entries(spec.outputs ?? {})) {
      withValue(v, (pv) => check(cd.defOutput(def, name, pv)))
    }
    /* The adapter supplies every required slot; redo only when the class has one, so
     * a class without it keeps redo-by-exec. */
    const slots = ['canRun', 'exec', 'undo', 'memSize', 'discard']
    if (typeof Cls.prototype.redo === 'function') slots.push('redo')
    const factory = {make: () => backend.foreignHandle(new Adapter(new Cls()))}
    check(cd.registerForeignCommand(ctx, def, backend.foreignHandle(factory), slots.join(' ')))
  } finally {
    def[Symbol.dispose]()
  }
}
