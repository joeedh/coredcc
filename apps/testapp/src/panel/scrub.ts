/** Dragging a number's name sideways changes it: one gesture, which Escape takes back. */
import type {PathMeta} from 'coredcc-script'
import type {WidgetHost} from './widgets.ts'

const KINDS = ['Int', 'Float', 'Component']

/** `of` gives what is dragged, when it begins; undefined for nothing. */
export function scrub(
  name: HTMLElement,
  host: WidgetHost,
  of: () => {meta: PathMeta; value: number} | undefined
) {
  name.addEventListener('pointerdown', (down) => {
    const from = of()
    if (down.button !== 0 || !from || !KINDS.includes(from.meta.kind)) return
    const integer = from.meta.kind === 'Int'
    const step = from.meta.step ?? (integer ? 1 : 0.0625)
    let gesture = 0
    const move = (e: PointerEvent) => {
      const value = from.value + (e.clientX - down.clientX) * step
      gesture ||= host.beginGesture()
      host.write(() => (integer ? Math.round(value) : value), gesture)
    }
    const end = () => {
      document.removeEventListener('pointermove', move)
      document.removeEventListener('pointerup', end)
      document.removeEventListener('keydown', key)
    }
    const key = (e: KeyboardEvent) => {
      if (e.key !== 'Escape') return
      end()
      if (gesture !== 0) host.cancelGesture(gesture)
    }
    document.addEventListener('pointermove', move)
    document.addEventListener('pointerup', end)
    document.addEventListener('keydown', key)
  })
}
