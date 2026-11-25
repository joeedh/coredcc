/**
 * The DOM widgets of properties, one for each kind of path (`meta().kind`). A widget shows a
 * value, and calls `write` when the user changes it.
 */
import {Ref, type NamedValue, type PathMeta, type Writable} from 'coredcc-script'

/** What a widget's user does. A gesture's writes are one undo step. */
export type Write = (value: Writable, gesture?: number) => void

export interface Widget {
  root: HTMLElement
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

let gestures = 0

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
  if (!/^\s*[+-]?\d+\s*$/.test(text)) throw new TypeError(`'${text}' isn't an integer`)
  const n = Number(text)
  return Number.isSafeInteger(n) ? n : BigInt(text.trim())
}

function parseFloatText(text: string): number {
  const n = text.trim() === '' ? NaN : Number(text)
  if (Number.isNaN(n)) throw new TypeError(`'${text}' isn't a number`)
  return n
}

/* A text or number field: written when the edit ends, and not overwritten during it. */
function field(parent: HTMLElement, type: string, commit: (text: string) => void) {
  const input = element('input', 'field', parent)
  input.type = type
  let editing = false
  input.addEventListener('input', () => (editing = true))
  input.addEventListener('change', () => {
    editing = false
    commit(input.value)
  })
  input.addEventListener('blur', () => (editing = false))
  return {
    input,
    show(text: string) {
      if (!editing) input.value = text
    },
  }
}

function numberInput(input: HTMLInputElement, meta: PathMeta, integer: boolean) {
  if (meta.hardMin !== undefined) input.min = String(meta.hardMin)
  if (meta.hardMax !== undefined) input.max = String(meta.hardMax)
  input.step = integer ? '1' : 'any'
}

function checkbox(parent: HTMLElement, write: (on: boolean) => void, label?: string) {
  const holder = label === undefined ? parent : element('label', 'check', parent)
  const input = element('input', '', holder)
  input.type = 'checkbox'
  if (label !== undefined) holder.append(label)
  input.addEventListener('change', () => write(input.checked))
  return input
}

function boolWidget(write: Write): Widget {
  const root = element('span', 'widget')
  const input = checkbox(root, write)
  return {
    root,
    inputs: new Map([['', input]]),
    show: (v) => void (input.checked = v === true),
    shown: () => input.checked,
  }
}

/* A range input: the moves of a drag are one gesture, which `change` ends. */
function sliderWidget(meta: PathMeta, write: Write): Widget {
  const root = element('span', 'widget')
  const input = element('input', 'slider', root)
  const text = element('output', '', root)
  input.type = 'range'
  input.min = String(meta.softMin ?? meta.hardMin)
  input.max = String(meta.softMax ?? meta.hardMax)
  const integer = meta.kind === 'Int'
  input.step = integer ? '1' : 'any'
  let gesture = 0
  input.addEventListener('input', () => {
    gesture ||= ++gestures
    write(integer ? parseInteger(input.value) : parseFloatText(input.value), gesture)
  })
  input.addEventListener('change', () => (gesture = 0))
  return {
    root,
    inputs: new Map([['', input]]),
    show(v) {
      input.value = String(v)
      text.value = String(v)
    },
    shown: () => Number(input.value),
  }
}

function numberWidget(meta: PathMeta, write: Write, integer: boolean): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'number', (text) => write(integer ? parseInteger(text) : parseFloatText(text)))
  numberInput(f.input, meta, integer)
  return {
    root,
    inputs: new Map([['', f.input]]),
    show: (v) => f.show(String(v)),
    shown: () => (integer ? parseInteger(f.input.value) : Number(f.input.value)),
  }
}

function textWidget(meta: PathMeta, write: Write): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'text', (text) => write(text))
  if (meta.maxLength !== undefined) f.input.maxLength = meta.maxLength
  return {
    root,
    inputs: new Map([['', f.input]]),
    show: (v) => f.show(String(v)),
    shown: () => f.input.value,
  }
}

function enumWidget(meta: PathMeta, write: Write): Widget {
  const root = element('span', 'widget')
  const select = element('select', '', root)
  for (const item of meta.items ?? []) {
    const option = element('option', '', select)
    option.value = item.key
    option.textContent = item.uiName
    if (item.description) option.title = item.description
  }
  select.addEventListener('change', () => write(select.value))
  return {
    root,
    inputs: new Map([['', select]]),
    show: (v) => void (select.value = typeof v === 'string' ? v : ''), /* a number has no item */
    shown: () => select.value,
  }
}

function flagsWidget(meta: PathMeta, write: Write): Widget {
  const root = element('span', 'widget')
  const inputs = new Map<string, HTMLInputElement>()
  const keys = () => [...inputs].filter(([, input]) => input.checked).map(([key]) => key)
  for (const item of meta.items ?? []) {
    inputs.set(
      item.key,
      checkbox(root, () => write(keys()), item.uiName)
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

function vectorWidget(meta: PathMeta, count: number, write: Write): Widget {
  const root = element('span', 'widget vector')
  const fields = Array.from({length: count}, (_, i) =>
    field(root, 'number', (text) => {
      const value = [...shown]
      value[i] = parseFloatText(text)
      write(value)
    })
  )
  let shown: number[] = new Array(count).fill(0)
  for (const f of fields) numberInput(f.input, meta, false)
  return {
    root,
    inputs: new Map(fields.map((f, i) => [String(i), f.input])),
    show(v) {
      shown = v as number[]
      fields.forEach((f, i) => f.show(String(shown[i])))
    },
    shown: () => fields.map((f) => Number(f.input.value)),
  }
}

/* A pointer as its text; "" is null. */
function pointerWidget(write: Write): Widget {
  const root = element('span', 'widget')
  const f = field(root, 'text', (text) => write(new Ref(text)))
  return {
    root,
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
export function makeWidget(meta: PathMeta, write: Write, options: WidgetOptions): Widget {
  const bounded = (meta.softMin ?? meta.hardMin) !== undefined && (meta.softMax ?? meta.hardMax) !== undefined
  const slider = bounded && (options.widget === 'slider' || meta.ui?.slider === 'Slider')
  switch (meta.kind) {
    case 'Bool':
    case 'EnumKey':
      return boolWidget(write)
    case 'Int':
    case 'Float':
      return slider ? sliderWidget(meta, write) : numberWidget(meta, write, meta.kind === 'Int')
    case 'Component':
      return numberWidget(meta, write, false)
    case 'String':
      return textWidget(meta, write)
    case 'Enum':
      return enumWidget(meta, write)
    case 'Flags':
      return flagsWidget(meta, write)
    case 'Vec2':
      return vectorWidget(meta, 2, write)
    case 'Vec3':
      return vectorWidget(meta, 3, write)
    case 'Vec4':
    case 'Quat':
      return vectorWidget(meta, 4, write)
    case 'Pointer':
      return pointerWidget(write)
    default: /* Length, Mat4, and what isn't a property */
      return readOnlyWidget()
  }
}
