/**
 * Saving and loading a document's blocks. None of it works in a slot, but `appendLibrary` in a
 * command's exec.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'

import {check, ScriptError, take} from './core.ts'

/** A block's id as text, as `core.block.duplicate(blocks=...)` takes it, and its name. */
export interface BlockEntry {
  block: string
  name: string
}
/** The first 10,000 of a list. */
export interface ReportList<T> {
  total: number
  first: T[]
}
/** Something a save or a load went on without. `block` is left out if it is about the file. */
export interface IoIssue extends Partial<BlockEntry> {
  /** A StatusCode's name. */
  code: string
  message: string
  suggestions: string[]
}

/** What a save did: lib/io/library_io.h's SaveReport. */
export interface SaveReport {
  blocks: number
  /** A number past 2^53 is a string. */
  loadBytes: number | string
  loadBlocks: number
  transient: ReportList<BlockEntry>
  throughTransient: ReportList<BlockEntry>
  hostPinned: ReportList<BlockEntry>
  detached: ReportList<BlockEntry>
  issues: ReportList<IoIssue>
}
/** What a load did: LoadReport. */
export interface LoadReport {
  records: number
  missingType: number
  missingBlock: number
  blocks: ReportList<BlockEntry>
  issues: ReportList<IoIssue>
}
export interface AppendReport extends LoadReport {
  /** The new ids, by those in the file. */
  ids: ReportList<{from: string; to: string}>
}

export interface LoadOptions {
  /** What placeholders are said to come from, such as the file's name. */
  sourceName?: string
  /** Bounds on what the file may take; without one, the default. */
  maxBytes?: number
  maxBlocks?: number
}

const limit = (what: string, n: number | undefined): bigint => {
  if (n === undefined) return 0n
  if (!Number.isSafeInteger(n) || n <= 0) throw new TypeError(`${what}: a positive integer, not ${n}`)
  return BigInt(n)
}

function withOptions<R>(opts: LoadOptions, fn: (name: string, bytes: bigint, blocks: bigint) => R): R {
  return fn(opts.sourceName ?? '', limit('maxBytes', opts.maxBytes), limit('maxBlocks', opts.maxBlocks))
}

/* Calls copy their spans, so large ones are read in parts. */
const CHUNK = 1 << 16

/** The document's blocks as bytes, and what the save did. */
export function saveLibrary(ctx: Context): {bytes: Uint8Array; report: SaveReport} {
  const r = cd.saveLibrary(ctx)
  try {
    if (!r.status.ok()) throw ScriptError.from(r.status)
    const bytes = new Uint8Array(Number(cd.bytesByteSize(r.value)))
    for (let at = 0; at < bytes.length; at += CHUNK) {
      check(cd.bytesRead(r.value, BigInt(at), bytes.subarray(at, at + CHUNK)))
    }
    return {bytes, report: JSON.parse(r.report) as SaveReport}
  } finally {
    r[Symbol.dispose]()
  }
}
/** Loads into a document without blocks. Not undoable. */
export function loadLibrary(ctx: Context, bytes: Uint8Array, opts: LoadOptions = {}): LoadReport {
  return withOptions(opts, (name, maxBytes, maxBlocks) =>
    JSON.parse(take(cd.loadLibrary(ctx, bytes, name, maxBytes, maxBlocks), (json) => json))
  )
}
/** Adds a file's blocks to the document, under new ids, as one undo step. */
export function appendLibrary(ctx: Context, bytes: Uint8Array, opts: LoadOptions = {}): AppendReport {
  return withOptions(opts, (name, maxBytes, maxBlocks) =>
    JSON.parse(take(cd.appendLibrary(ctx, bytes, name, maxBytes, maxBlocks), (json) => json))
  )
}
/** Reads the blocks that were loaded before their types were registered. The history must be
 * empty (`clearHistory`). */
export function resolvePlaceholders(ctx: Context): LoadReport {
  return JSON.parse(take(cd.resolvePlaceholders(ctx), (json) => json))
}
/** Forgets what could be undone and redone. */
export function clearHistory(ctx: Context) {
  check(cd.clearHistory(ctx))
}
