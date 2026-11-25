/**
 * The testbus methods of the property panel (TA2). What they do to a widget they do through DOM
 * events, as the user would.
 */
import type {Context} from 'coredcc-script'
import {Panel} from '../panel/panel.ts'

export type DefineMethod = (name: string, fn: (...args: any[]) => unknown) => void

/** A file of `apps/testapp/layouts`, by name. */
function layoutFile(name: string): string {
  if (!/^[\w-]+\.xml$/.test(name)) throw new TypeError(`'${name}' isn't the name of a layout file`)
  const request = new XMLHttpRequest() /* a page's own file, at once */
  request.open('GET', new URL(`../layouts/${name}`, location.href), false)
  request.send()
  return request.responseText
}

/** Registers the methods over `ctx()`, the document's. Returns what closes the panel, which must
 * be called before the document closes. */
export function installPanelMethods(method: DefineMethod, ctx: () => Context): () => void {
  let panel: Panel | undefined
  const close = () => {
    panel?.dispose()
    panel = undefined
  }
  const current = (): Panel => {
    if (!panel) throw new Error('there is no panel')
    return panel
  }
  /* The input `at` (a component, or a flag's key; '' for the only one) of a widget. */
  const inputOf = (id: string, at: unknown = '') => {
    const widget = current().entry(id).widget
    const input = widget?.inputs.get(String(at))
    if (!input) throw new Error(`'${id}' has no input '${at}'`)
    if (input.disabled) throw new Error(`'${id}' is disabled`)
    return input
  }
  const set = (input: HTMLInputElement | HTMLSelectElement, value: unknown, events: string[]) => {
    if (input.type === 'checkbox' || input.type === 'radio') {
      ;(input as HTMLInputElement).checked = Boolean(value)
    } else {
      input.value = String(value)
      if (input.type !== 'range' && input.value !== String(value)) {
        throw new Error(`the input can't hold '${value}'`)
      }
    }
    for (const type of events) input.dispatchEvent(new Event(type, {bubbles: true}))
    current().rethrow()
  }

  /* Shows a layout, replacing the one there is, which stays if this one isn't a layout. */
  method('panel', (layout: {xml?: string; file?: string}) => {
    const xml = layout.file !== undefined ? layoutFile(layout.file) : layout.xml
    if (typeof xml !== 'string') throw new TypeError('panel: expected {xml} or {file}')
    const next = new Panel(ctx(), xml, document.getElementById('panel')!)
    close()
    panel = next
    return {issues: panel.issues}
  })
  method('panelState', (opts: {flush?: boolean} = {}) => {
    if (opts.flush !== false) current().refresh()
    return current().state()
  })
  method('panelSet', (id: string, value: unknown, at?: string | number) => {
    set(inputOf(id, at), value, ['input', 'change'])
    return true
  })
  /* Typing, without its end: `panelSet` and `panelBlur` end it. */
  method('panelType', (id: string, text: string, at?: string | number) => {
    set(inputOf(id, at), text, ['input'])
    return true
  })
  method('panelBlur', (id: string, at?: string | number) => {
    inputOf(id, at).dispatchEvent(new Event('blur'))
    current().rethrow()
    return true
  })
  const escape = (on: EventTarget) =>
    on.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', bubbles: true}))
  /* A drag of a slider over `values`, as one gesture. With `cancel`, Escape is pressed, and the
   * slider moves on. Without `end`, it goes on. */
  method('panelDrag', (id: string, values: unknown[], opts: {cancel?: boolean; end?: boolean} = {}) => {
    const input = inputOf(id)
    for (const v of values) set(input, v, ['input'])
    if (opts.cancel) {
      escape(input)
      set(input, values[0], ['input'])
    }
    if (opts.end !== false) set(input, input.value, ['change'])
    return true
  })
  /* A drag of a number's name, by `dxs` pixels from its start. */
  method('panelScrub', (id: string, dxs: number[], opts: {cancel?: boolean} = {}) => {
    const name = current().entry(id).el.querySelector(':scope > .name')
    if (!name) throw new Error(`'${id}' has no name`)
    const pointer = (on: EventTarget, type: string, x: number) =>
      on.dispatchEvent(new PointerEvent(type, {clientX: 100 + x, button: 0, bubbles: true}))
    pointer(name, 'pointerdown', 0)
    for (const x of dxs) pointer(document, 'pointermove', x)
    if (opts.cancel) {
      escape(document)
      pointer(document, 'pointermove', dxs[0])
    }
    pointer(document, 'pointerup', dxs.at(-1) ?? 0)
    pointer(document, 'pointermove', 500)
    current().rethrow()
    return true
  })
  /* A tool's button, or a tab's. */
  method('panelClick', (id: string) => {
    const button = current().entry(id).button
    if (!button) throw new Error(`'${id}' has no button`)
    if (button.disabled) throw new Error(`'${id}' is disabled`)
    button.click()
    current().rethrow()
    return true
  })
  return close
}
