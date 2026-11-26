/**
 * A property panel: the DOM of a layout, over a document. It reads its paths with one
 * PathReader, and shows again what `flushChanges` finds changed.
 */
import * as cdcc from 'coredcc-script'
import type {Context, LayoutCondition, LayoutIssue, LayoutNode, NamedValue, PathChange, PathMeta} from 'coredcc-script'
import {scrub} from './scrub.ts'
import {caption, InputError, makeWidget, type Widget, type WidgetHost} from './widgets.ts'

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
  /** Why a tool can't run now. */
  reason?: string
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
  /** Its visibleIf, and whether what its path names is there. */
  wanted: boolean
  there: boolean
  /** Why a tool can't run now. */
  reason?: string
  /** Its button's title otherwise. */
  title?: string
  path?: string
  meta?: PathMeta
  widget?: Widget
  /** Its meta changed while its widget was in use. */
  stale?: boolean
  button?: HTMLButtonElement
  /** Of a tab: called when it was shown or hidden. */
  shown?: () => void
  holder?: HTMLElement
  error?: string
}

type Show = (v: PathChange, newMeta: boolean) => void

/* A Custom's path is the host's to decide about. */
const containers = new Set(['layout', 'column', 'row', 'panel', 'tabs', 'tab'])

const message = (e: unknown) => (e instanceof Error ? e.message : String(e))

export class Panel {
  readonly issues: LayoutIssue[]
  private readonly entries = new Map<string, Entry>()
  private readonly root: Entry
  private readonly reader: cdcc.PathReader
  /** By the reader's index. */
  private readonly shows: Show[][] = []
  private last: PathChange[] = []
  private readonly checks: cdcc.InvocationChecks
  /** By InvocationChecks' index. */
  private readonly commandEntries: Entry[] = []
  /** In preorder, as the issues count nodes. */
  private nodes = 0
  private readonly indices = new Map<string, number>()
  private readonly timer: ReturnType<typeof setInterval>
  /** The ids of the layout, and those made for the nodes without one. */
  private readonly ids = new Set<string>()
  private uiNames: Map<string, string> | undefined
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
    this.checks = new cdcc.InvocationChecks(ctx)
    try {
      this.takeIds(layout)
      this.root = this.build(layout, undefined)
      this.reader.watch()
      for (const v of this.reader.takeAll({names: true})) this.show(v, true)
      this.checkCommands()
    } catch (e) {
      this.reader[Symbol.dispose]()
      this.checks[Symbol.dispose]()
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
    this.checks[Symbol.dispose]()
    this.root.el.remove()
  }

  /** Shows what has changed. */
  refresh() {
    cdcc.flushChanges(this.ctx)
    for (const v of this.reader.takeChanges({names: true})) this.show(v, v.metaChanged)
    this.checkCommands()
  }

  /* What a tool's canRun looks at isn't known, so each is asked. */
  private checkCommands() {
    for (const {index, reason} of this.checks.changed()) {
      const e = this.commandEntries[index]
      e.reason = reason
      e.button!.title = reason ?? e.title ?? ''
      this.applyEnabled(e)
    }
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
    const id = node.id ?? this.newId(node.kind)
    const e: Entry = {id, node, el, parent, children: [], enabled: true, enabledHere: true, wanted: true, there: true}
    /* The props of a commandOptions aren't the layout's. */
    const issues = this.issues.filter((i) => i.node === this.nodes && i.severity === 'Error')
    if (parent?.node.kind !== 'commandOptions') this.nodes++
    el.className = ['node', node.kind, node.expand ? 'expand' : '', node.class ?? ''].filter((c) => c).join(' ')
    el.dataset.id = id
    if (node.description) el.title = node.description
    for (const [k, v] of Object.entries(node.attrs ?? {})) el.setAttribute(`data-attr-${k}`, v)
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
      case 'command':
        this.command(e)
        break
      case 'commandOptions':
        this.commandOptions(e)
        break
      case 'label':
        caption(el, node.icon, node.label ?? '')
        break
      case 'separator':
        el.append(document.createElement('hr'))
        break
      case 'custom':
        el.dataset.tag = node.tag
        if (node.text !== undefined) el.textContent = node.text
        break
    }
    if (node.kind !== 'tabs') for (const child of node.children ?? []) within.append(this.build(child, e).el)
    if (node.visibleIf) {
      this.condition(node.visibleIf, (on) => {
        e.wanted = on
        this.applyVisible(e)
      })
    }
    const own = node.path !== undefined && containers.has(node.kind)
    if (own && !issues.some((i) => /^<\w+> path:/.test(i.message))) {
      this.watch(node.effectivePath!, (v) => {
        e.there = cdcc.containerIsThere(v)
        this.applyVisible(e)
      })
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
    caption(summary, e.node.icon, e.node.label ?? '')
    details.append(summary)
    e.el.append(details)
    return details
  }

  private tabs(e: Entry) {
    const bar = document.createElement('div')
    bar.className = 'bar'
    e.el.append(bar)
    /* The one that the user chose, or the first, if that is hidden. */
    let wanted: Entry | undefined
    const select = () => {
      const visible = e.children.filter((tab) => !tab.el.hidden)
      const chosen = wanted && visible.includes(wanted) ? wanted : visible[0]
      for (const tab of e.children) {
        tab.el.classList.toggle('chosen', tab === chosen)
        tab.button?.classList.toggle('chosen', tab === chosen)
        if (tab.button) tab.button.hidden = tab.el.hidden
      }
    }
    for (const child of e.node.children ?? []) {
      const tab = this.build(child, e)
      const button = document.createElement('button')
      caption(button, child.icon, child.label ?? '')
      button.addEventListener('click', () => {
        wanted = tab
        select()
      })
      tab.button = button
      tab.shown = select
      bar.append(button)
      e.el.append(tab.el)
    }
    select()
  }

  /* A prop's widget is made when its path has a value, and again when its meta changes. */
  private prop(e: Entry, path: string) {
    e.path = path
    const label = document.createElement('label')
    label.className = 'name'
    const holder = document.createElement('span')
    holder.className = 'holder'
    e.holder = holder
    e.el.append(label, holder)
    const host: WidgetHost = {
      write: (make, gesture) => this.act(e, () => cdcc.set(this.ctx, path, make(), gesture)),
      beginGesture: () => cdcc.beginGesture(this.ctx),
      cancelGesture: (gesture) => this.act(e, () => cdcc.cancelGesture(this.ctx, gesture)),
      done: () => this.act(e, () => {}),
    }
    let current: NamedValue
    this.watch(path, (v, newMeta) => {
      if (v.error) {
        e.widget = e.meta = undefined
        e.error = v.error.message
        holder.replaceChildren(v.error.message)
      } else {
        current = v.value
        e.stale ||= newMeta
        if (!e.widget || (e.stale && !e.widget.active?.())) {
          if (!e.widget) e.error = undefined
          e.stale = false
          e.meta = cdcc.meta(this.ctx, path)
          e.widget = makeWidget(e.meta, host, {widget: e.node.widget})
          holder.replaceChildren(e.widget.root)
          e.el.title = e.node.description ?? e.meta.description ?? ''
          this.applyEnabled(e)
        }
        e.widget.show(v.value)
      }
      const name = e.node.label ?? e.meta?.uiName ?? path
      caption(label, e.node.icon ?? e.meta?.icon, e.node.showLabel === false ? '' : name, e.node.iconsOnly)
    })
    scrub(label, host, () => {
      const usable = e.widget && ![...e.widget.inputs.values()].some((input) => input.disabled)
      return usable && e.meta && typeof current === 'number' ? {meta: e.meta, value: current} : undefined
    })
  }

  private command(e: Entry) {
    const button = document.createElement('button')
    const id = e.node.invocation!.split('(')[0].trim()
    caption(button, e.node.icon, e.node.label ?? this.commandName(id), e.node.iconsOnly)
    button.addEventListener('click', () =>
      this.act(e, () => void cdcc.invokeAt(this.ctx, e.node.prefix ?? '', e.node.invocation!))
    )
    e.button = button
    e.title = button.title
    e.el.append(button)
    this.commandEntries[this.checks.add(e.node.prefix ?? '', e.node.invocation!)] = e
  }

  private commandName(id: string): string {
    this.uiNames ??= new Map(cdcc.apiCatalog(this.ctx).commands.map((c) => [c.id, c.uiName]))
    return this.uiNames.get(id) ?? id
  }

  private takeIds(node: LayoutNode) {
    if (node.id !== undefined) this.ids.add(node.id)
    node.children?.forEach((child) => this.takeIds(child))
  }

  /* One that no node has. */
  private newId(base: string): string {
    let id = base
    for (let i = 1; this.ids.has(id); i++) id = `${base}-${i}`
    this.ids.add(id)
    return id
  }

  /* The command's last-used inputs. */
  private commandOptions(e: Entry) {
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
      const node: LayoutNode = {kind: 'prop', id: this.newId(`${e.id}.${name}`), effectivePath: `${path}.${name}`}
      e.el.append(this.build(node, e).el)
    }
  }

  /* ---- showing ---- */

  private watch(path: string, show: Show) {
    let i = this.indices.get(path)
    if (i === undefined) {
      i = this.reader.add(path)
      this.indices.set(path, i)
      this.shows[i] = []
    }
    this.shows[i].push(show)
  }

  private show(v: PathChange, newMeta = false) {
    this.last[v.index] = v
    for (const show of this.shows[v.index]) show(v, newMeta)
  }

  private condition(c: LayoutCondition, apply: (on: boolean) => void) {
    this.watch(c.path, (v) => apply(cdcc.conditionHolds(c, v)))
  }

  private applyVisible(e: Entry) {
    e.el.hidden = !(e.wanted && e.there)
    e.shown?.()
  }

  private applyEnabled(e: Entry) {
    e.enabled = (e.parent?.enabled ?? true) && e.enabledHere
    e.el.classList.toggle('disabled', !e.enabled)
    if (e.button && e.node.kind === 'command') e.button.disabled = !e.enabled || e.reason !== undefined
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
        if (!(err instanceof cdcc.ScriptError || err instanceof InputError)) throw err
        e.error = err.message
      }
      e.el.classList.toggle('error', e.error !== undefined)
      this.refresh()
      /* A refused write, or one that was clamped, leaves what the user typed. */
      const i = e.path === undefined ? undefined : this.indices.get(e.path)
      if (i !== undefined && e.widget) this.show(this.last[i])
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
    if (e.reason !== undefined) out.reason = e.reason
    if (e.children.length > 0) out.children = e.children.map((c) => this.stateOf(c))
    return out
  }
}
