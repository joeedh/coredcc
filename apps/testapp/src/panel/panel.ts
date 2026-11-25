/**
 * A property panel: the DOM of a layout, over a document. It reads its paths with one
 * PathReader, and shows again what `flushChanges` finds changed.
 */
import * as cdcc from 'coredcc-script'
import type {Context, LayoutCondition, LayoutIssue, LayoutNode, NamedValue, PathMeta, Writable} from 'coredcc-script'
import {makeWidget, type Widget} from './widgets.ts'

/** What a node shows, read from the DOM. */
export interface NodeState {
  id: string
  kind: string
  label?: string
  visible: boolean
  enabled: boolean
  /** Of a prop: its path's kind, and what its widget shows. */
  path?: string
  valueKind?: string
  value?: unknown
  /** Why a prop has no value, or why the last write or run failed. */
  error?: string
  children?: NodeState[]
}

/* A node of the layout on the page. */
interface Entry {
  id: string
  node: LayoutNode
  el: HTMLElement
  parent?: Entry
  children: Entry[]
  enabled: boolean
  /** Its own condition, and that of what it is in. */
  enabledHere: boolean
  path?: string
  meta?: PathMeta
  widget?: Widget
  button?: HTMLButtonElement
  holder?: HTMLElement
  error?: string
}

type Shown = {value: NamedValue} | {error: string}

const message = (e: unknown) => (e instanceof Error ? e.message : String(e))

export class Panel {
  readonly issues: LayoutIssue[]
  private readonly entries = new Map<string, Entry>()
  private readonly root: Entry
  private readonly reader: cdcc.PathReader
  /** By the reader's index. */
  private readonly shows: ((v: Shown) => void)[][] = []
  private readonly indices = new Map<string, number>()
  private readonly timer: ReturnType<typeof setInterval>
  private count = 0
  /** An error that isn't the document's: a bug, or a dead module. */
  private thrown: unknown

  /** Throws, with the container as it was, if `xml` isn't a layout. */
  constructor(
    private readonly ctx: Context,
    xml: string,
    container: HTMLElement
  ) {
    const layout = cdcc.parseLayout(xml)
    this.issues = cdcc.validateLayout(ctx, xml)
    this.reader = new cdcc.PathReader(ctx)
    try {
      this.root = this.build(layout, undefined)
      this.reader.watch()
      this.shows.forEach((_, i) => this.show(i))
    } catch (e) {
      this.reader[Symbol.dispose]()
      throw e
    }
    container.replaceChildren(this.root.el)
    this.timer = setInterval(() => {
      try {
        this.refresh()
      } catch (e) {
        console.error('[panel] stopped:', e)
        clearInterval(this.timer)
      }
    }, 50)
  }

  dispose() {
    clearInterval(this.timer)
    this.reader[Symbol.dispose]()
    this.root.el.remove()
  }

  /** Shows what has changed. */
  refresh() {
    cdcc.flushChanges(this.ctx)
    for (const i of this.reader.takeChanged()) this.show(i)
  }

  state(): NodeState {
    return this.stateOf(this.root)
  }

  entry(id: string): Entry {
    const e = this.entries.get(id)
    if (!e) throw new Error(`the panel has no '${id}'`)
    return e
  }

  /** Throws what a handler couldn't: the DOM drops a handler's exceptions. */
  rethrow() {
    const e = this.thrown
    this.thrown = undefined
    if (e !== undefined) throw e
  }

  /* ---- building ---- */

  private build(node: LayoutNode, parent: Entry | undefined): Entry {
    const el = document.createElement('div')
    const id = node.id ?? `n${this.count}`
    this.count++
    const e: Entry = {id, node, el, parent, children: [], enabled: true, enabledHere: true}
    el.className = ['node', node.kind, node.class ?? ''].join(' ').trim()
    el.dataset.id = id
    if (node.description) el.title = node.description
    this.entries.set(id, e)
    parent?.children.push(e)
    let within: HTMLElement = el
    switch (node.kind) {
      case 'panel':
        within = this.panel(e)
        break
      case 'tabs':
        this.tabs(e)
        break
      case 'prop':
        this.prop(e, node.effectivePath!)
        break
      case 'tool':
        this.tool(e)
        break
      case 'toolOptions':
        this.toolOptions(e)
        break
      case 'label':
        el.textContent = node.label ?? ''
        break
      case 'separator':
        el.append(document.createElement('hr'))
        break
      case 'custom':
        el.dataset.tag = node.tag
        for (const [k, v] of Object.entries(node.attrs ?? {})) el.setAttribute(`data-attr-${k}`, v)
        if (node.text !== undefined) el.textContent = node.text
        break
    }
    if (node.kind !== 'tabs') for (const child of node.children ?? []) within.append(this.build(child, e).el)
    if (node.visibleIf) {
      this.condition(node.visibleIf, (on) => void (el.hidden = !on))
    }
    if (node.enabledIf) {
      this.condition(node.enabledIf, (on) => {
        e.enabledHere = on
        this.applyEnabled(e)
      })
    }
    return e
  }

  private panel(e: Entry): HTMLElement {
    const details = document.createElement('details')
    details.open = !e.node.closed
    const summary = document.createElement('summary')
    summary.textContent = e.node.label ?? ''
    details.append(summary)
    e.el.append(details)
    return details
  }

  private tabs(e: Entry) {
    const bar = document.createElement('div')
    bar.className = 'bar'
    e.el.append(bar)
    const select = (chosen: Entry) => {
      e.children.forEach((tab, i) => {
        tab.el.classList.toggle('chosen', tab === chosen)
        bar.children[i].classList.toggle('chosen', tab === chosen)
      })
    }
    for (const child of e.node.children ?? []) {
      const tab = this.build(child, e)
      const button = document.createElement('button')
      button.textContent = child.label ?? ''
      button.addEventListener('click', () => select(tab))
      tab.button = button
      bar.append(button)
      e.el.append(tab.el)
    }
    if (e.children.length > 0) select(e.children[0])
  }

  /* A prop's widget comes when its path has a value, and again when that comes back. */
  private prop(e: Entry, path: string) {
    e.path = path
    const label = document.createElement('label')
    label.className = 'name'
    const holder = document.createElement('span')
    holder.className = 'holder'
    e.holder = holder
    e.el.append(label, holder)
    this.watch(path, (v) => {
      if ('error' in v) {
        e.widget = e.meta = undefined
        e.error = v.error
        holder.replaceChildren(v.error)
      } else {
        if (!e.widget) {
          e.error = undefined
          e.meta = cdcc.meta(this.ctx, path)
          const write = (value: Writable, gesture?: number) => {
            this.act(e, () => cdcc.set(this.ctx, path, value, gesture))
          }
          e.widget = makeWidget(e.meta, write, {widget: e.node.widget})
          holder.replaceChildren(e.widget.root)
          this.applyEnabled(e)
        }
        e.widget.show(v.value)
      }
      label.textContent = e.node.showLabel === false ? '' : (e.node.label ?? e.meta?.uiName ?? path)
    })
  }

  private tool(e: Entry) {
    const button = document.createElement('button')
    button.textContent = e.node.label ?? e.node.invocation!.split('(')[0]
    button.addEventListener('click', () =>
      this.act(e, () => void cdcc.invokeAt(this.ctx, e.node.prefix ?? '', e.node.invocation!))
    )
    e.button = button
    e.el.append(button)
  }

  /* The command's last-used inputs. */
  private toolOptions(e: Entry) {
    const path = e.node.optionsPath!
    let members: string[]
    try {
      members = cdcc.meta(this.ctx, path).members ?? []
    } catch (err) {
      e.error = message(err)
      e.el.textContent = e.error
      return
    }
    for (const name of members) {
      const node: LayoutNode = {kind: 'prop', id: `${e.id}.${name}`, effectivePath: `${path}.${name}`}
      e.el.append(this.build(node, e).el)
    }
  }

  /* ---- showing ---- */

  private watch(path: string, show: (v: Shown) => void) {
    let i = this.indices.get(path)
    if (i === undefined) {
      i = this.reader.add(path)
      this.indices.set(path, i)
      this.shows[i] = []
    }
    this.shows[i].push(show)
  }

  private show(i: number) {
    let v: Shown
    try {
      v = {value: this.reader.value(i, {names: true})}
    } catch (e) {
      if (!(e instanceof cdcc.ScriptError)) throw e
      v = {error: e.message}
    }
    for (const show of this.shows[i]) show(v)
  }

  /* A condition without a value is false, also when it is negated. */
  private condition(c: LayoutCondition, apply: (on: boolean) => void) {
    this.watch(c.path, (v) => apply('value' in v && (v.value === true) !== (c.not === true)))
  }

  private applyEnabled(e: Entry) {
    e.enabled = (e.parent?.enabled ?? true) && e.enabledHere
    e.el.classList.toggle('disabled', !e.enabled)
    if (e.button && e.node.kind === 'tool') e.button.disabled = !e.enabled
    for (const input of e.widget?.inputs.values() ?? []) {
      input.disabled = !e.enabled || e.meta?.readOnly === true
    }
    for (const child of e.children) this.applyEnabled(child)
  }

  /* ---- what the user does ---- */

  private act(e: Entry, run: () => void) {
    try {
      e.error = undefined
      try {
        run()
      } catch (err) {
        if (!(err instanceof cdcc.ScriptError)) throw err
        e.error = err.message
      }
      e.el.classList.toggle('error', e.error !== undefined)
      this.refresh()
      /* A refused write, or one that was clamped, leaves what the user typed. */
      const i = e.path === undefined ? undefined : this.indices.get(e.path)
      if (i !== undefined && e.widget) this.show(i)
    } catch (err) {
      this.thrown = err
      throw err
    }
  }

  private stateOf(e: Entry): NodeState {
    const out: NodeState = {id: e.id, kind: e.node.kind, visible: !e.el.hidden, enabled: e.enabled}
    const label = e.el.querySelector(':scope > label, :scope > details > summary')
    const text = e.button?.textContent ?? label?.textContent ?? (e.node.kind === 'label' ? e.el.textContent : '')
    if (text) out.label = text
    if (e.path !== undefined) out.path = e.path
    if (e.meta) out.valueKind = e.meta.kind
    if (e.widget) {
      out.value = e.widget.shown()
      const inputs = [...e.widget.inputs.values()]
      if (inputs.length > 0) out.enabled = inputs.every((input) => !input.disabled)
    }
    if (e.error !== undefined) out.error = e.error
    if (e.children.length > 0) out.children = e.children.map((c) => this.stateOf(c))
    return out
  }
}
