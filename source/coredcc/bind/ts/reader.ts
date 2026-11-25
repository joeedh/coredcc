/**
 * Reading and watching many paths at once.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {PathReader as RawReader} from 'coredcc-gen/coredcc/PathReader'

import {check, checkCtx, disposeWith, forget, take, toJs, toNamed, type Disposable, type NamedValue} from './core.ts'

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
  /** The paths that changed since the last call, as of the last `flushChanges`. */
  takeChanged(): number[] {
    const out: number[] = []
    for (let i = cd.readerTakeChanged(this.reader); i >= 0; i = cd.readerTakeChanged(this.reader)) out.push(i)
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
