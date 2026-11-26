/**
 * Layouts in TS: a layout's nodes, and its issues. `cdcc.invokeAt` runs a command of one.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'
import {StatusCode} from 'coredcc-gen/coredcc/StatusCode'

import {take} from './core.ts'
import type {PathChange} from './reader.ts'

export type LayoutKind =
  | 'layout' | 'column' | 'row' | 'panel' | 'tabs' | 'tab' | 'prop' | 'command' | 'commandOptions'
  | 'label' | 'separator' | 'custom'

/** A path that a node is shown or enabled by. `not` negates it. */
export interface LayoutCondition {
  path: string
  not?: boolean
}

/** A node of a layout (layout/layout.h's layoutToJson). Paths in `effectivePath`, `prefix` and
 * the conditions are whole: they start from the root. */
export interface LayoutNode {
  kind: LayoutKind
  id?: string
  class?: string
  label?: string
  icon?: string
  description?: string
  /** As written. */
  path?: string
  effectivePath?: string
  /** Of a commandOptions: what has the inputs that its command last ran with as members. */
  optionsPath?: string
  visibleIf?: LayoutCondition
  enabledIf?: LayoutCondition
  widget?: string
  /** The root's: the JSON's, 1. */
  version?: number
  /** Of a command: for `invokeAt(ctx, node.prefix ?? '', node.invocation)`. */
  invocation?: string
  target?: string
  prefix?: string
  closed?: boolean
  expand?: boolean
  iconsOnly?: boolean
  showLabel?: boolean
  /** Of a custom node. */
  tag?: string
  text?: string
  /** The host's own: `data-` ones, and any of a custom node. */
  attrs?: Record<string, string>
  children?: LayoutNode[]
}

export interface LayoutIssue {
  severity: 'Error' | 'Warning'
  /** From 1. */
  line: number
  column: number
  /** Its index, in preorder. */
  node: number
  /** A StatusCode's name. */
  code: string
  message: string
  suggestions?: string[]
}

/** Whether the condition holds, as layout.h's conditionHolds, given its path's value from a
 * PathReader: of a Bool, a pointer, a number or a String. Without a value it doesn't. */
export function conditionHolds(condition: LayoutCondition, path: PathChange): boolean {
  if (path.error || path.type === 'None') return false
  const plain = path.type === 'Bool' || path.type === 'Int' || path.type === 'String' || path.type === 'Pointer'
  const on = plain && Boolean(path.value)
  return on !== (condition.not === true)
}
/** Whether a container with a `path` is to be shown, given its effectivePath's value from a
 * PathReader: see layout.h, "What isn't there". */
export function containerIsThere(path: PathChange): boolean {
  if (path.error) return path.error.code === StatusCode.Unsupported
  return path.type !== 'Pointer' || path.value !== null
}

/** The layout in `xml`. Throws a ScriptError with the position if it isn't one. */
export function parseLayout(xml: string): LayoutNode {
  return JSON.parse(take(cd.parseLayout(xml), (json) => json)) as LayoutNode
}

/** What is wrong with the layout in `xml`, for `ctx`'s root: what it names that doesn't exist or
 * doesn't fit. Warnings are about what can't be checked. */
export function validateLayout(ctx: Context, xml: string): LayoutIssue[] {
  return JSON.parse(take(cd.validateLayout(ctx, xml), (json) => json)) as LayoutIssue[]
}
