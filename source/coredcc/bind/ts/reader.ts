/**
 * Reading and watching many paths at once.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PathReader as RawReader} from 'coredcc-gen/coredcc/PathReader'

import {
  check,
  checkCtx,
  disposeWith,
  forget,
  Ref,
  ScriptError,
  take,
  toJs,
  toNamed,
  wideNumber,
  type Disposable,
  type NamedValue,
} from './core.ts'

/** The types of values, as C++ has them; 'None' without a value. */
export type ValueType = 'None' | 'Bool' | 'Int' | 'Float' | 'String' | 'Enum' | 'Flags' | 'Vector' | 'Pointer'

/** What became of a path. */
export interface PathChange {
  index: number
  /** Whether the value changed, or the path has an error now, had one, or has another. */
  changed: boolean
  /** Whether its `meta` may have changed: make its widget again. */
  metaChanged: boolean
  /** Of the value: an enum has the same in either form. */
  type: ValueType
  value: NamedValue
  /** Why there is no value. */
  error?: ScriptError
}

type Entry = [index: number, what: number, kind: string, value: unknown, more?: unknown, text?: string]

const types: Record<string, ValueType> = {
  b: 'Bool', i: 'Int', f: 'Float', s: 'String', e: 'Enum', g: 'Flags', v: 'Vector', p: 'Pointer',
}
const floats: Record<string, number> = {nan: NaN, inf: Infinity, '-inf': -Infinity}
const toFloat = (x: unknown) => (typeof x === 'string' ? floats[x] : (x as number))

function decode([index, what, kind, value, more, text]: Entry, names: boolean): PathChange {
  const out: PathChange = {
    index,
    changed: (what & 1) !== 0,
    metaChanged: (what & 2) !== 0,
    type: types[kind] ?? 'None',
    value: undefined,
  }
  switch (kind) {
    case 'n':
      break
    case 'f':
      out.value = toFloat(value)
      break
    case 'v':
      out.value = (value as unknown[]).map(toFloat)
      break
    case 'i':
      out.value = wideNumber(value as number | string)
      break
    case 'p':
      out.value = value === null ? null : new Ref(value as string)
      break
    case 'e':
    case 'g':
      out.value = names && more !== undefined ? (more as string | string[]) : wideNumber(value as number | string)
      break
    case '!':
      out.error = new ScriptError(value as number, more as string, text)
      break
    default: /* 'b' and 's' */
      out.value = value as boolean | string
  }
  return out
}

/**
 * A list of paths, and their values when they were last read: by `read`, or by `flushChanges`
 * when they are watched. Made with a session's context, and disposed of with the session at
 * the latest.
 */
export class PathReader implements Disposable {
  private raw: RawReader | undefined
  private readonly ctx: Context
  private readonly paths: string[] = []
  constructor(ctx: Context, paths: Iterable<string> = []) {
    checkCtx(ctx)
    this.ctx = ctx
    disposeWith(ctx, this)
    this.raw = cd.newPathReader()
    try {
      for (const path of paths) this.add(path)
    } catch (e) {
      this[Symbol.dispose]()
      throw e
    }
  }
  private get reader(): RawReader {
    if (!this.raw) throw new Error('the PathReader was disposed')
    return this.raw
  }
  get count(): number {
    return this.paths.length
  }
  /** Its index. Throws if it doesn't parse. */
  add(path: string): number {
    const i = take(cd.readerAdd(this.reader, path), Number)
    this.paths.push(path)
    return i
  }
  read() {
    check(cd.readerRead(this.ctx, this.reader))
  }
  /** The value of path `i`, as `get` gives it. Throws the error if it has none. */
  value(i: number, opts: {names?: boolean} = {}): NamedValue {
    return take(cd.readerValue(this.reader, i), (v) =>
      opts.names ? toNamed(this.ctx, this.paths[i], v) : toJs(this.ctx, v)
    )
  }
  /** Reads the paths, and watches them from now on. Again after adding some. Not from a slot. */
  watch() {
    check(cd.readerWatch(this.ctx, this.reader))
  }
  unwatch() {
    cd.readerUnwatch(this.reader)
  }
  /** The paths that changed since they were last taken, as of the last `flushChanges`, with their
   * values: one call for all, which is what a frame should use. */
  takeChanges(opts: {names?: boolean} = {}): PathChange[] {
    return this.entries(false, opts.names ?? false)
  }
  /** Every path, as takeChanges gives them: after `read` or `watch`. */
  takeAll(opts: {names?: boolean} = {}): PathChange[] {
    return this.entries(true, opts.names ?? false)
  }
  private entries(all: boolean, names: boolean): PathChange[] {
    const json = take(cd.readerTake(this.ctx, this.reader, all), (s) => s)
    return (JSON.parse(json) as Entry[]).map((e) => decode(e, names))
  }
  /** The paths that changed since they were last taken, as of the last `flushChanges`. */
  takeChanged(): number[] {
    const out: number[] = []
    for (let i = cd.readerTakeChanged(this.reader); i >= 0; i = cd.readerTakeChanged(this.reader)) out.push(i)
    return out
  }
  /** As takeChanged, for paths whose `meta` may have changed: make their widgets again. A path
   * may be in either, or both. */
  takeChangedMeta(): number[] {
    const out: number[] = []
    const next = () => cd.readerTakeChangedMeta(this.reader)
    for (let i = next(); i >= 0; i = next()) out.push(i)
    return out
  }
  [Symbol.dispose]() {
    const raw = this.raw
    if (!raw) return
    this.raw = undefined
    forget(this.ctx, this)
    cd.readerUnwatch(raw)
    raw[Symbol.dispose]()
  }
}

/** Reads the watched paths that may have changed, and tells the watchers: once per frame, say.
 * How many had changed. Not from a script's slot. */
export function flushChanges(ctx: Context): number {
  return take(cd.flushChanges(ctx), Number)
}
