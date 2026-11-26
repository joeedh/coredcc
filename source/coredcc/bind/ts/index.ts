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
import type {Status} from 'coredcc-gen/coredcc/Status'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

import {
  bagToJs,
  check,
  checkCtx,
  newErrorStatus,
  inSlot,
  isFatal,
  ScriptError,
  statusOf,
  sync,
  take,
  toJs,
  toNamed,
  withValue,
  type Disposable,
  type NamedValue,
  type Value,
  type Writable,
} from './core.ts'

export {CommandFlags, StatusCode}
export type {Context}
export {float, FloatValue, fromJs, load, quat, QuatValue, Ref, ScriptError, Session, toJs, type NamedValue, type Value, type Writable} from './core.ts'
export * from './catalog.ts'
export * from './graph.ts'
export * from './layout.ts'
export * from './library.ts'
export * from './reader.ts'
export * from './run.ts'
export * from './scene.ts'
export * from './tool.ts'
export * from './drawlist.ts'
export * from './typed.ts'

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
  /** A hook may change the range, the items and the access: see PathReader's takeChangedMeta. */
  dynamic?: boolean
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
  /** A family of units; values are in its base unit. */
  unit?: string
  /** What to show them in. */
  displayUnit?: string
  /** The components' names, of a vector. */
  components?: string[]
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
/** `value` as text in its meta's display unit: "1.25 m". Of a meta without a unit, the number. */
export function formatUnit(value: number, meta: Pick<PathMeta, 'unit' | 'displayUnit' | 'precision'>): string {
  return take(cd.formatUnit(value, meta.unit ?? '', meta.displayUnit ?? '', meta.precision ?? 3), (s) => s)
}
/** The number that a user typed, in base units: "1.5", in the meta's display unit, or "2 cm".
 * Throws a ScriptError if it is none. */
export function parseUnit(text: string, meta: Pick<PathMeta, 'unit' | 'displayUnit'>): number {
  return take(cd.parseUnit(text, meta.unit ?? '', meta.displayUnit ?? ''), (x) => x)
}
/** With `names`, an enum reads as its key and flags as their keys; a number without an item
 * stays a number. */
export function get(ctx: Context, path: string): Value
export function get(ctx: Context, path: string, opts: {names?: boolean}): NamedValue
export function get(ctx: Context, path: string, opts: {names?: boolean} = {}): NamedValue {
  return take(cd.getValue(ctx, path), (v) => (opts.names ? toNamed(ctx, path, v) : toJs(ctx, v)))
}
/** The value at `path` as text, in the active scene's units: "1.5 cm". */
export function format(ctx: Context, path: string): string {
  return take(cd.formatValue(ctx, path), (text) => text)
}
/** What to set `path` to for text that a user typed, as "1 m, 20 cm, 0" for a vector. A number
 * without a unit is in the active scene's. Nothing is set. */
export function parse(ctx: Context, path: string, text: string): Value {
  return take(cd.parseValue(ctx, path, text), (v) => toJs(ctx, v))
}
/** An undoable set; from a command's exec, a sub-command. Sets of one member with the same
 * `gesture` (one slider drag, from beginGesture) are one undo step. */
export function set(ctx: Context, path: string, value: Writable, gesture?: number) {
  if (gesture !== undefined && !(Number.isSafeInteger(gesture) && gesture > 0)) {
    throw new TypeError(`set: a gesture is a positive integer, not ${gesture}`)
  }
  withValue(ctx, value, (v) =>
    check(gesture === undefined ? cd.setValue(ctx, path, v) : cd.setValueGesture(ctx, path, v, BigInt(gesture)))
  )
}
/** A new gesture's id: for the sets of one drag. */
export function beginGesture(ctx: Context): number {
  return take(cd.beginGesture(ctx), Number)
}
/** Takes back what a gesture's sets did, if no other command ran since (Escape). Not redoable. */
export function cancelGesture(ctx: Context, gesture: number) {
  check(cd.cancelGesture(ctx, BigInt(gesture)))
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
export function commandInput(ctx: Context, name: string): Value {
  return take(cd.commandInput(ctx, name), (v) => toJs(ctx, v))
}
/** Sets the running command's output `name` (inside a slot). */
export function setCommandOutput(ctx: Context, name: string, value: Writable) {
  withValue(ctx, value, (v) => check(cd.setCommandOutput(ctx, name, v)))
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
 * - `byteSize` (bytes held for undo) and `discard` are optional; a throw from either is
 *   ignored (reported with console.warn).
 */
export interface ScriptCommand {
  canRun?(ctx: Context): void | boolean | string
  exec(ctx: Context): void
  undo(ctx: Context): void
  redo?(ctx: Context): void
  byteSize?(): number
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
  canRun(ctx: Context): Status | undefined {
    return inSlot(() => {
      try {
        const r = sync('canRun', this.cmd.canRun?.(ctx))
        if (typeof r === 'string') return newErrorStatus(StatusCode.FailedPrecondition, r)
        if (r === false) return newErrorStatus(StatusCode.FailedPrecondition, 'refused')
        return undefined
      } catch (e) {
        return statusOf(StatusCode.FailedPrecondition, e)
      }
    })
  }
  exec(ctx: Context): Status | undefined {
    return inSlot(() => {
      try {
        sync('exec', this.cmd.exec(ctx))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  undo(ctx: Context) {
    inSlot(() => sync('undo', this.cmd.undo(ctx)))
  }
  redo(ctx: Context): Status | undefined {
    return inSlot(() => {
      try {
        sync('redo', this.cmd.redo?.(ctx))
        return undefined
      } catch (e) {
        return statusOf(StatusCode.Internal, e)
      }
    })
  }
  /* A failing byteSize or discard would report an unrelated applied run as failed. */
  byteSize(): number {
    return inSlot(() => {
      try {
        const n = Number(sync('byteSize', this.cmd.byteSize?.() ?? 0))
        return Number.isFinite(n) && n > 0 ? Math.min(Math.floor(n), Number.MAX_SAFE_INTEGER) : 0
      } catch (e) {
        if (isFatal(e)) throw e
        console.warn(`${this.id}: byteSize failed, counted as 0:`, e)
        return 0
      }
    })
  }
  discard() {
    inSlot(() => {
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
export function defineCommand(ctx: Context, spec: CommandSpec, Cls: new () => ScriptCommand) {
  checkCtx(ctx)
  if (typeof Cls !== 'function' || typeof Cls.prototype !== 'object') {
    throw new TypeError(`defineCommand: ${spec.id}: expected a class, not ${typeof Cls}`)
  }
  const flags = spec.flags ?? 0
  if (!Number.isInteger(flags) || flags < 0 || flags > 0xffffffff) {
    throw new TypeError(`defineCommand: flags must be CommandFlags bits, not ${flags}`)
  }
  const backend = getBindingBackend()
  const def = cd.newCommandDef(spec.id)
  if (!def) throw new Error(`defineCommand: no def for ${spec.id}`)
  try {
    if (spec.uiName !== undefined) check(cd.commandDefUiName(def, spec.uiName))
    if (spec.description !== undefined) check(cd.commandDefDescription(def, spec.description))
    if (spec.icon !== undefined) check(cd.commandDefIcon(def, spec.icon))
    check(cd.commandDefFlags(def, flags))
    for (const [name, v] of Object.entries(spec.inputs ?? {})) {
      withValue(ctx, v, (pv) => check(cd.commandDefInput(def, name, pv)))
    }
    for (const [name, v] of Object.entries(spec.outputs ?? {})) {
      withValue(ctx, v, (pv) => check(cd.commandDefOutput(def, name, pv)))
    }
    /* The adapter supplies every required slot; redo only when the class has one, so
     * a class without it keeps redo-by-exec. */
    const hasRedo = typeof Cls.prototype.redo === 'function'
    const slots = ['canRun', 'exec', 'undo', 'byteSize', 'discard']
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
