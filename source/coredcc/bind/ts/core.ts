/**
 * What the parts of coredcc's TS layer share: loading, sessions, errors, and values. Hosts
 * import index.ts, which exports the public ones.
 */
import {
  BindingManager,
  createWasmHelpers,
  getBindingBackend,
  setBindingBackend,
  WasmBindingBackend,
  type IBindingBackend,
} from '@litestl/typescript-runtime'
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PropertyBag} from 'coredcc-gen/coredcc/PropertyBag'
import type {PropValue} from 'coredcc-gen/coredcc/PropValue'
import {PropValueType} from 'coredcc-gen/coredcc/PropValueType'
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'


/**
 * A pointer value: what it points to, as text (`api/ref_text.h`), e.g.
 * `@block(coredcc, <32 hex digits>) : Mesh`. Frozen. Two with the same text are the same
 * pointer.
 */
export class Ref {
  readonly text: string
  constructor(text: string) {
    if (typeof text !== 'string') throw new TypeError(`Ref: expected a string, not ${typeof text}`)
    this.text = text
    Object.freeze(this)
  }
  toString() {
    return this.text
  }
}

/** A value as scripts read it. Vectors, quaternions and Mat4 are arrays; an Int past
 * 2^53 is a bigint; a pointer is a Ref, or null; None is `undefined`. */
export type Value = boolean | number | bigint | string | number[] | Ref | null | undefined
/** With `{names: true}`: an enum's key, and a flags' keys. */
export type NamedValue = Value | string[]

/** A Float, even when the number is whole: `inputs: {factor: float(1)}`. */
export class FloatValue {
  constructor(readonly value: number) {}
}
export const float = (value: number) => new FloatValue(value)

/** A rotation, xyzw. Written to a Quat, an array of 4 numbers does as well. */
export class QuatValue {
  readonly value: readonly number[]
  constructor(x: number, y: number, z: number, w: number) {
    this.value = [x, y, z, w]
  }
}
export const quat = (x: number, y: number, z: number, w: number) => new QuatValue(x, y, z, w)

/** A value scripts can write. A whole number is an Int (use `float()` for a Float); an array
 * of strings is flags by their keys (`[]` for none); null is the null pointer. Arrays of 2-4
 * numbers are vectors, and of 16 a Mat4 (column by column); `quat()` makes a Quat. */
export type Writable =
  | boolean
  | number
  | bigint
  | string
  | ArrayLike<number>
  | readonly string[]
  | FloatValue
  | QuatValue
  | Ref
  | null
  | PropValue

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

export interface Disposable {
  [Symbol.dispose](): void
}
export interface ResultOf<T> extends Disposable {
  status: Status
  value: T
}

/** The key of `value` in a table of names by bound enum item: the inverse of `get`. */
export function keyOf<K, V>(table: ReadonlyMap<K, V>, value: V): K | undefined {
  for (const [k, v] of table) if (v === value) return k
  return undefined
}

/** Throws a ScriptError for a failed Status, and disposes of it either way. */
export function check(s: Status) {
  try {
    if (!s.ok()) throw ScriptError.from(s)
  } finally {
    s[Symbol.dispose]()
  }
}

/** `read(r.value)` for an ok result; a ScriptError otherwise. Disposes of `r`. */
export function take<T, J>(r: ResultOf<T>, read: (v: T) => J): J {
  try {
    if (!r.status.ok()) throw ScriptError.from(r.status)
    return read(r.value)
  } finally {
    r[Symbol.dispose]()
  }
}

export const isFatal = (e: unknown) =>
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

/* Script slots (of commands and graph kernels) in progress: a document can't close under
 * one. */
export const scriptSlots = {depth: 0}

/* What goes with a session's context, before its host. */
const owners = new WeakMap<Context, Set<Disposable>>()

/** Disposes of `owned` with the session that `ctx` is of, at the latest. */
export function disposeWith(ctx: Context, owned: Disposable) {
  const set = owners.get(ctx)
  if (!set) throw new Error("expected a session's context")
  set.add(owned)
}
export function forget(ctx: Context, owned: Disposable) {
  owners.get(ctx)?.delete(owned)
}

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
    owners.set(ctx, new Set())
    Object.defineProperty(ctx, '__host', {value: host, enumerable: false})
  }
  get disposed() {
    return this.host === undefined
  }
  /** Closes the document. Not from inside a script command's slot. */
  [Symbol.dispose]() {
    if (!this.host) return
    if (scriptSlots.depth > 0) throw new Error("Session: can't close the document from inside a command")
    for (const owned of [...(owners.get(this.ctx) ?? [])]) owned[Symbol.dispose]()
    owners.delete(this.ctx)
    cd.disableDrivers(this.ctx)[Symbol.dispose]() /* before the Library goes */
    ;(this.ctx as unknown as {ptr: number}).ptr = 0
    const host = this.host
    this.host = undefined
    host[Symbol.dispose]()
  }
}

/** The loaded module's memory now (it moves when memory grows): for zero-copy views. */
export function moduleHeap(): Uint8Array | undefined {
  const heap = (loaded?.module as {HEAPU8?: unknown} | undefined)?.HEAPU8
  return heap instanceof Uint8Array ? heap : undefined
}

/** Throws unless `ctx` still addresses a live context. */
export function checkCtx(ctx: Context) {
  if (!(ctx as unknown as {ptr?: number}).ptr) throw new Error('the context was disposed')
}

/* ---- values ---- */

export function copyValue(v: PropValue): PropValue {
  return getBindingBackend().constructWith('coredcc::PropValue', 'copy', v) as PropValue
}

/** A bigint only past 2^53, where a number would lose digits. */
export const toNumber = (n: bigint): number | bigint => (n >= -(2n ** 53n) && n <= 2n ** 53n ? Number(n) : n)
/** An integer as JSON has it: past 2^53, a string. */
export const wideNumber = (n: number | string): number | bigint => (typeof n === 'number' ? n : toNumber(BigInt(n)))

/** Graph sockets and params hold no pointers, so a node has no context to read them with. */
const pointerCtx = (ctx: Context | undefined): Context => {
  if (!ctx) throw new TypeError('a graph value is never a pointer')
  return ctx
}

/** A JS form of `v`, copied out of it. */
export function toJs(ctx: Context | undefined, v: PropValue): Value {
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
    case PropValueType.Pointer: {
      const text = take(cd.pointerText(pointerCtx(ctx), v), (x) => x)
      return text === '' ? null : new Ref(text)
    }
    default:
      throw new TypeError(`toJs: a value of an unknown type (${v.type()})`)
  }
}

/** As toJs, but an enum is its key, and flags their keys. A value that no item has is the number
 * instead: an enum's items may be dynamic, and a document may hold one that they no longer have. */
export function toNamed(ctx: Context, path: string, v: PropValue): NamedValue {
  const flags = v.type() === PropValueType.Flags
  if (!flags && v.type() !== PropValueType.Enum) return toJs(ctx, v)
  try {
    const keys = take(cd.valueKeys(ctx, path, v), (s) => s)
    return !flags ? keys : keys === '' ? [] : keys.split('|')
  } catch (e) {
    if (e instanceof ScriptError && e.code === StatusCode.NotFound) return toJs(ctx, v)
    throw e
  }
}

const isBoundValue = (x: unknown): x is PropValue => getBindingBackend().typeName(x) === 'coredcc::PropValue'
const isArrayLike = (x: unknown): x is ArrayLike<number> =>
  Array.isArray(x) || (ArrayBuffer.isView(x) && !(x instanceof DataView))

const INT64_MIN = -(2n ** 63n)
const INT64_MAX = 2n ** 63n - 1n

function mat4(c: number[]): PropValue {
  const columns = [0, 4, 8, 12].map((i) => cd.makeVec4(c[i], c[i + 1], c[i + 2], c[i + 3]))
  try {
    return take(cd.makeMat4(columns[0], columns[1], columns[2], columns[3]), copyValue)
  } finally {
    for (const v of columns) v[Symbol.dispose]()
  }
}

/** An owned PropValue for `x`. A whole number is an Int, unless it is past 2^53 (then
 * a Float, which C++ range-checks); `float()` makes a Float. Bound PropValues are
 * copied. */
export function fromJs(ctx: Context | undefined, x: Writable): PropValue {
  if (x === null) return cd.makeNullPointer()
  if (x instanceof Ref) return take(cd.makePointer(pointerCtx(ctx), x.text), copyValue)
  if (typeof x === 'boolean') return cd.makeBool(x)
  if (typeof x === 'bigint') {
    if (x < INT64_MIN || x > INT64_MAX) throw new RangeError(`fromJs: ${x} is outside int64`)
    return cd.makeInt(x)
  }
  if (typeof x === 'number') {
    return Number.isSafeInteger(x) ? cd.makeInt(BigInt(x)) : cd.makeFloat(x)
  }
  if (typeof x === 'string') return take(cd.makeString(x), copyValue)
  if (x instanceof FloatValue) return cd.makeFloat(x.value)
  if (x instanceof QuatValue) return cd.makeQuat(x.value[0], x.value[1], x.value[2], x.value[3])
  if (Array.isArray(x) && x.every((k) => typeof k === 'string')) {
    if (x.length === 0) return cd.makeFlags(0n)
    const odd = x.find((k) => k === '' || k.includes('|'))
    if (odd !== undefined) throw new TypeError(`fromJs: '${odd}' is no flag's key`)
    return take(cd.makeString(x.join('|')), copyValue)
  }
  if (isArrayLike(x)) {
    const c = Array.from(x as ArrayLike<number>, Number)
    if (c.length === 2) return cd.makeVec2(c[0], c[1])
    if (c.length === 3) return cd.makeVec3(c[0], c[1], c[2])
    if (c.length === 4) return cd.makeVec4(c[0], c[1], c[2], c[3])
    if (c.length === 16) return mat4(c)
    throw new TypeError(`fromJs: a vector has 2-4 components, and a Mat4 16, not ${c.length}`)
  }
  if (isBoundValue(x)) return copyValue(x)
  throw new TypeError(`fromJs: can't make a value of ${String(x)}`)
}

export function withValue<R>(ctx: Context | undefined, x: Writable, fn: (v: PropValue) => R): R {
  const v = fromJs(ctx, x)
  try {
    return fn(v)
  } finally {
    v[Symbol.dispose]()
  }
}

export function bagToJs(ctx: Context, bag: PropertyBag): Record<string, Value> {
  const out: Record<string, Value> = {}
  const n = cd.bagSize(bag)
  for (let i = 0; i < n; i++) {
    const name = take(cd.bagName(bag, i), (s) => s)
    /* defineProperty: an output named __proto__ is data, not the prototype. */
    const value = take(cd.bagGet(bag, name), (v) => toJs(ctx, v))
    Object.defineProperty(out, name, {value, enumerable: true, writable: true, configurable: true})
  }
  return out
}

/* ---- slots ---- */

/** An error Status for a slot's return, disposed of after C++ copies it. */
export function errorStatus(code: StatusCode, message: string): Status {
  const s = cd.statusError(code, message)
  queueMicrotask(() => s[Symbol.dispose]())
  return s
}

export function statusOf(code: StatusCode, e: unknown): Status {
  if (isFatal(e)) throw e
  if (e instanceof ScriptError) return errorStatus(e.code, e.detail)
  return errorStatus(code, e instanceof Error ? e.message : String(e))
}

const isThenable = (v: unknown) =>
  (typeof v === 'object' || typeof v === 'function') && v !== null && typeof (v as {then?: unknown}).then === 'function'

/** `r`, unless it is a Promise: slots are synchronous. */
export function sync<T>(slot: string, r: T): T {
  if (isThenable(r)) {
    ;(r as unknown as Promise<unknown>).catch(() => {}) /* reported here, not as unhandled */
    throw new TypeError(`${slot} returned a Promise: slots are synchronous`)
  }
  return r
}

/** Runs a slot's body. */
export function inSlot<T>(fn: () => T): T {
  scriptSlots.depth++
  try {
    return fn()
  } finally {
    scriptSlots.depth--
  }
}
