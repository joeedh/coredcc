/**
 * Running commands with JS values, asking whether they would run, and driving interactive ones.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import type {RunInputs} from 'coredcc-gen/coredcc/RunInputs'
import type {Status} from 'coredcc-gen/coredcc/Status'
import type {InvocationChecks as RawChecks} from 'coredcc-gen/coredcc/InvocationChecks'

import {
  bagToJs,
  check,
  checkCtx,
  disposeWith,
  forget,
  ScriptError,
  take,
  withValue,
  type Disposable,
  type Value,
  type Writable,
} from './core.ts'

export interface RunOptions {
  /** A path prefix, as a layout's tool has (see invokeAt). Without one, none. */
  prefix?: string
  /** Inputs that aren't given have their last-used values: by default. */
  lastUsed?: boolean
}

function withInputs<R>(ctx: Context, values: Record<string, Writable | undefined>, fn: (inputs: RunInputs) => R): R {
  const inputs = cd.newRunInputs()
  try {
    for (const [name, v] of Object.entries(values)) {
      if (v !== undefined) withValue(ctx, v, (pv) => check(cd.runInputsSet(inputs, name, pv)))
    }
    return fn(inputs)
  } finally {
    inputs[Symbol.dispose]()
  }
}

/** The error's text, or undefined. */
function reason(s: Status): string | undefined {
  try {
    check(s)
    return undefined
  } catch (e) {
    if (e instanceof ScriptError) return e.detail
    throw e
  }
}

/** Runs command `id`, returning its outputs. Inputs are JS values, so nothing has to be quoted;
 * those that are undefined aren't given. From a command's exec, a sub-command. */
export function run(
  ctx: Context,
  id: string,
  inputs: Record<string, Writable | undefined> = {},
  opts: RunOptions = {}
): Record<string, Value> {
  return withInputs(ctx, inputs, (raw) =>
    take(cd.runAt(ctx, opts.prefix ?? '', id, raw, opts.lastUsed ?? true), (bag) => bagToJs(ctx, bag))
  )
}

/** Why `run`, or `beginInteractive`, would refuse now: for a tooltip. undefined if it would start.
 * It asks the command's `canRun`, so it is only as cheap as that. From a slot it always refuses. */
export function canRun(
  ctx: Context,
  id: string,
  inputs: Record<string, Writable | undefined> = {},
  opts: RunOptions = {}
): string | undefined {
  return withInputs(ctx, inputs, (raw) => reason(cd.canRunAt(ctx, opts.prefix ?? '', id, raw, opts.lastUsed ?? true)))
}

/** As canRun, for `invokeAt(ctx, prefix, invocation)`. */
export function canInvoke(ctx: Context, prefix: string, invocation: string): string | undefined {
  return reason(cd.canInvokeAt(ctx, prefix, invocation))
}

/** The invocations of a UI: canInvoke of each in one call. Disposed of with the session at the latest. */
export class InvocationChecks implements Disposable {
  private raw: RawChecks | undefined
  constructor(private readonly ctx: Context) {
    checkCtx(ctx)
    disposeWith(ctx, this)
    this.raw = cd.newInvocationChecks()
  }
  private get checks(): RawChecks {
    if (!this.raw) throw new Error('the InvocationChecks were disposed')
    return this.raw
  }
  /** Its index. Throws if the prefix or the invocation doesn't parse. */
  add(prefix: string, invocation: string): number {
    return take(cd.invocationChecksAdd(this.checks, prefix, invocation), Number)
  }
  /** Those whose answer changed since the last call, or that are new, with why they can't run. */
  changed(): {index: number; reason?: string}[] {
    const json = take(cd.invocationChecksRun(this.ctx, this.checks), (s) => s)
    return (JSON.parse(json) as [number, string | null][]).map(([index, why]) =>
      why === null ? {index} : {index, reason: why}
    )
  }
  [Symbol.dispose]() {
    const raw = this.raw
    if (!raw) return
    this.raw = undefined
    forget(this.ctx, this)
    raw[Symbol.dispose]()
  }
}

/* ---- interactive commands ---- */

/**
 * Starts an Interactive command: its `exec` captures what `undo` needs. Then `writeInteractive`
 * changes the document, until `commitInteractive` makes it one undo step, or `cancelInteractive`
 * (or `undo`) takes it back. Until then no other command runs. Not from a slot, as the calls
 * below.
 */
export function beginInteractive(
  ctx: Context,
  id: string,
  inputs: Record<string, Writable | undefined> = {},
  opts: RunOptions = {}
) {
  withInputs(ctx, inputs, (raw) =>
    check(cd.beginInteractiveAt(ctx, opts.prefix ?? '', id, raw, opts.lastUsed ?? true))
  )
}
/** The open command's id. */
export function interactive(ctx: Context): string | undefined {
  return take(cd.interactiveId(ctx), (id) => id) || undefined
}
/** An unrecorded write as if from the open command, so under its prefix. */
export function writeInteractive(ctx: Context, path: string, value: Writable) {
  withValue(ctx, value, (v) => check(cd.writeInteractive(ctx, path, v)))
}
export function commitInteractive(ctx: Context) {
  check(cd.commitInteractive(ctx))
}
export function cancelInteractive(ctx: Context) {
  check(cd.cancelInteractive(ctx))
}

/* ---- the last command ---- */

/** The last command in the history, if it can be run again with other inputs: not an interactive
 * one, nor a set, nor after an undo. */
export function lastCommand(ctx: Context): {id: string; inputs: Record<string, Value>} | undefined {
  const id = take(cd.lastCommandId(ctx), (s) => s)
  return id === '' ? undefined : {id, inputs: take(cd.lastCommandInputs(ctx), (bag) => bagToJs(ctx, bag))}
}
/** Undoes it and runs it again where it ran, with `inputs` over those it had. If that fails, it
 * stays as it was, and this throws. */
export function rerunLast(ctx: Context, inputs: Record<string, Writable | undefined>) {
  withInputs(ctx, inputs, (raw) => check(cd.rerunLast(ctx, raw)))
}
