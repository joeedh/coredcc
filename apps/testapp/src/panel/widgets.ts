/**
 * The DOM widgets of properties, one for each kind of path (`meta().kind`). A widget shows a
 * value, and calls `write` when the user changes it.
 */
import {Ref, type NamedValue, type PathMeta, type Writable} from 'coredcc-script'

/** What isn't a value: shown at the widget, as an error of the document's is. */
export class InputError extends Error {}

/** What a widget's user does. */
export interface WidgetHost {
  /** Writes the value that `make` gives, unless it throws an InputError. A gesture's writes are
   * one undo step. */
  write(make: () => Writable, gesture?: number): void
  /** A drag begins. */
  beginGesture(): number
  /** Takes back what a drag did. */
  cancelGesture(gesture: number): void
  /** A drag ended, or an edit: show the document's value. */
  done(): void
}

export interface Widget {
  root: HTMLElement
  /** In the middle of an edit or a drag: not to be replaced now. */
  active?(): boolean
  /** The elements that take input, by component or flag key; '' for the only one. */
  inputs: Map<string, HTMLInputElement | HTMLSelectElement>
  show(value: NamedValue): void
  /** What it shows now, read from its elements. */
  shown(): unknown
}

export interface WidgetOptions {
  /** The layout's hint. */
  widget?: string
}

function element<K extends keyof HTMLElementTagNameMap>(
  tag: K,
  className = '',
  parent?: HTMLElement
): HTMLElementTagNameMap[K] {
  const el = document.createElement(tag)
  if (className) el.className = className
  parent?.append(el)
  return el
}

/** An Int as it is written: a bigint past 2^53. */
function parseInteger(text: string): number | bigint {
  if (!/^\s*[+-]?\d+\s*$/.test(text)) throw new InputError(`'${text}' isn't an integer`)
  const n = Number(text)
  return Number.isSafeInteger(n) ? n : BigInt(text.trim())
}

function parseFloatText(text: string): number {
  const n = text.trim() === '' ? NaN : Number(text)
  if (Number.isNaN(n)) throw new InputError(`'${text}' isn't a number`)
  return n
}

/** An icon, by its name, and a text. With `iconsOnly` the text is a tooltip, if there is an icon. */
export function caption(el: HTMLElement, icon: string | undefined, text: string, iconsOnly = false) {
  el.replaceChildren()
  if (icon) element('span', 'icon', el).dataset.icon = icon
  if (icon && iconsOnly) el.title = text
  else el.append(text)
}

/* A text or number field: written when the edit ends, and not overwritten during it. An edit may
 * end without a change: the host then shows what it would have shown. */
function field(parent: HTMLElement, type: string, host: WidgetHost, make: (text: string) => Writable) {
  const input = element('input', 'field', parent)
  input.type = type
  let editing = false
  input.addEventListener('input', () => (editing = true))
  input.addEventListener('change', () => {
    editing = false
    host.write(() => make(input.value))
  })
  input.addEventListener('blur', () => {
    editing = false
    host.done()
  })
  return {
    input,
    active: () => editing,
    show(text: string) {
      if (!editing) input.value = text
    },
  }
}

/* A number, or what else is in the field. */
function shownNumber(input: HTMLInputElement): number | bigint | string {
  const text = input.value
  return /^-?\d+$/.test(text) ? parseInteger(text) : text.trim() === '' ? text : Number(text)
}

function numberInput(input: HTMLInputElement, meta: PathMeta, integer: boolean) {
  if (meta.hardMin !== undefined) input.min = String(meta.hardMin)
  if (meta.hardMax !== undefined) input.max = String(meta.hardMax)
  input.step = integer ? '1' : 'any'
}

type Item = NonNullable<PathMeta['items']>[number]

/* A checkbox, or a radio button, alone or with an item's caption. */
function checkbox(parent: HTMLElement, changed: (on: boolean) => void, item?: Item, iconsOnly = false) {
  const holder = item ? element('label', 'check', parent) : parent
  const input = element('input')
  input.type = 'checkbox'
  if (item) {
    caption(holder, item.icon, item.uiName, iconsOnly)
    if (item.description && !holder.title) holder.title = item.description
  }
  holder.prepend(input)
  input.addEventListener('change', () => changed(input.checked))
  return input
}

function boolWidget(host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const input = checkbox(root, (on) => host.write(() => on))
  return {
    root,
    inputs: new Map([['', input]]),
    show: (v) => void (input.checked = v === true),
    shown: () => input.checked,
  }
}

/* A range input: the moves of a drag are one gesture, which `change` ends, and Escape takes back. */
function sliderWidget(meta: PathMeta, host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const input = element('input', 'slider', root)
  const text = element('output', '', root)
  input.type = 'range'
  input.min = String(meta.softMin ?? meta.hardMin)
  input.max = String(meta.softMax ?? meta.hardMax)
  const integer = meta.kind === 'Int'
  input.step = integer ? '1' : 'any'
  let gesture = 0
  let cancelled = false
  input.addEventListener('input', () => {
    if (cancelled) return
    gesture ||= host.beginGesture()
    host.write(() => (integer ? parseInteger(input.value) : parseFloatText(input.value)), gesture)
  })
  input.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape' || gesture === 0) return
    host.cancelGesture(gesture)
    gesture = 0
    cancelled = true
  })
  input.addEventListener('change', () => {
    gesture = 0
    cancelled = false
    host.done()
  })
  return {
    root,
    active: () => gesture !== 0,
    inputs: new Map([['', input]]),
    show(v) {
      input.value = String(v)
      text.value = String(v)
    },
    shown: () => Number(input.value),
  }
}

function numberWidget(meta: PathMeta, host: WidgetHost, integer: boolean): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'number', host, integer ? parseInteger : parseFloatText)
  numberInput(f.input, meta, integer)
  return {
    root,
    inputs: new Map([['', f.input]]),
    active: f.active,
    show: (v) => f.show(String(v)),
    shown: () => shownNumber(f.input),
  }
}

function textWidget(meta: PathMeta, host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'text', host, (text) => text)
  if (meta.maxLength !== undefined) f.input.maxLength = meta.maxLength
  return {
    root,
    active: f.active,
    inputs: new Map([['', f.input]]),
    show: (v) => f.show(String(v)),
    shown: () => f.input.value,
  }
}

function enumWidget(meta: PathMeta, host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const select = element('select', '', root)
  for (const item of meta.items ?? []) {
    const option = element('option', '', select)
    option.value = item.key
    option.textContent = item.uiName
    if (item.description) option.title = item.description
  }
  select.addEventListener('change', () => host.write(() => select.value))
  return {
    root,
    inputs: new Map([['', select]]),
    show: (v) => void (select.value = typeof v === 'string' ? v : ''), /* a number has no item */
    shown: () => select.value,
  }
}

/* An enum as a button for each item. */
function choiceWidget(meta: PathMeta, host: WidgetHost): Widget {
  const root = element('span', 'widget choice')
  const inputs = new Map<string, HTMLInputElement>()
  for (const item of meta.items ?? []) {
    const input = checkbox(root, () => host.write(() => item.key), item, meta.ui?.iconsOnly)
    input.type = 'radio'
    inputs.set(item.key, input)
  }
  return {
    root,
    inputs,
    show(v) {
      for (const [key, input] of inputs) input.checked = key === v
    },
    shown: () => [...inputs].find(([, input]) => input.checked)?.[0] ?? '',
  }
}

function flagsWidget(meta: PathMeta, host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const inputs = new Map<string, HTMLInputElement>()
  const keys = () => [...inputs].filter(([, input]) => input.checked).map(([key]) => key)
  for (const item of meta.items ?? []) {
    inputs.set(
      item.key,
      checkbox(root, () => host.write(keys), item, meta.ui?.iconsOnly)
    )
  }
  return {
    root,
    inputs,
    show(v) {
      for (const [key, input] of inputs) input.checked = Array.isArray(v) && (v as string[]).includes(key)
    },
    shown: keys,
  }
}

function vectorWidget(meta: PathMeta, count: number, host: WidgetHost): Widget {
  const root = element('span', 'widget vector')
  const fields = Array.from({length: count}, (_, i) =>
    field(root, 'number', host, (text) => {
      const value = [...shown]
      value[i] = parseFloatText(text)
      return value
    })
  )
  let shown: number[] = new Array(count).fill(0)
  for (const f of fields) numberInput(f.input, meta, false)
  return {
    root,
    active: () => fields.some((f) => f.active()),
    inputs: new Map(fields.map((f, i) => [String(i), f.input])),
    show(v) {
      shown = v as number[]
      fields.forEach((f, i) => f.show(String(shown[i])))
    },
    shown: () => fields.map((f) => shownNumber(f.input)),
  }
}

/* A pointer as its text; "" is null. */
function pointerWidget(host: WidgetHost): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'text', host, (text) => new Ref(text))
  return {
    root,
    active: f.active,
    inputs: new Map([['', f.input]]),
    show: (v) => f.show(v === null ? '' : String(v)),
    shown: () => f.input.value,
  }
}

function readOnlyWidget(): Widget {
  const root = element('span', 'widget')
  const text = element('output', '', root)
  return {
    root,
    inputs: new Map(),
    show: (v) => void (text.value = Array.isArray(v) ? v.join(', ') : String(v)),
    shown: () => text.value,
  }
}

/** The widget for a path. */
export function makeWidget(meta: PathMeta, host: WidgetHost, options: WidgetOptions): Widget {
  const bounded = (meta.softMin ?? meta.hardMin) !== undefined && (meta.softMax ?? meta.hardMax) !== undefined
  const slider = bounded && (options.widget === 'slider' || meta.ui?.slider === 'Slider')
  switch (meta.kind) {
    case 'Bool':
    case 'EnumKey':
      return boolWidget(host)
    case 'Int':
    case 'Float':
      return slider ? sliderWidget(meta, host) : numberWidget(meta, host, meta.kind === 'Int')
    case 'Component':
      return numberWidget(meta, host, false)
    case 'String':
      return textWidget(meta, host)
    case 'Enum':
      return options.widget === 'buttons' || meta.ui?.expand ? choiceWidget(meta, host) : enumWidget(meta, host)
    case 'Flags':
      return flagsWidget(meta, host)
    case 'Vec2':
      return vectorWidget(meta, 2, host)
    case 'Vec3':
      return vectorWidget(meta, 3, host)
    case 'Vec4':
    case 'Quat':
      return vectorWidget(meta, 4, host)
    case 'Pointer':
      return pointerWidget(host)
    default: /* Length, Mat4, and what isn't a property */
      return readOnlyWidget()
  }
}
