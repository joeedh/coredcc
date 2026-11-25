/**
 * Layouts in TS: a layout's nodes, and its issues. `cdcc.invokeAt` runs a tool of one.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'

import {take} from './core.ts'

export type LayoutKind =
  | 'layout' | 'column' | 'row' | 'panel' | 'tabs' | 'tab' | 'prop' | 'tool' | 'toolOptions'
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
  /** Of a toolOptions: what has the inputs that its command last ran with as members. */
  optionsPath?: string
  visibleIf?: LayoutCondition
  enabledIf?: LayoutCondition
  widget?: string
  /** Of a tool: for `invokeAt(ctx, node.prefix ?? '', node.invocation)`. */
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

/** The layout in `xml`. Throws a ScriptError with the position if it isn't one. */
export function parseLayout(xml: string): LayoutNode {
  return JSON.parse(take(cd.layoutJson(xml), (json) => json)) as LayoutNode
}

/** What is wrong with the layout in `xml`, for `ctx`'s root: what it names that doesn't exist or
 * doesn't fit. Warnings are about what can't be checked. */
export function validateLayout(ctx: Context, xml: string): LayoutIssue[] {
  return JSON.parse(take(cd.validateLayoutXml(ctx, xml), (json) => json)) as LayoutIssue[]
}
