import {initGpu} from './gpu.ts'
import {installLogMirror} from './log.ts'
import {argValue, hasArg, isNw} from './node.ts'
import {installTestbus, registerMethod, setBackendInfo} from './testbus.ts'

installLogMirror()

if (isNw && !hasArg('apptest-headless')) {
  const win = (globalThis as any).nw.Window.get()
  win.show()
  if (hasArg('devtools')) win.showDevTools()
}

/* The wasm backend is its own bundle (src/backend/wasm.ts), built only when the wasm-st
 * module is; without it, or without --apptest-module, the app runs with no backend. */
async function loadBackend(): Promise<void> {
  const modulePath = argValue('apptest-module')
  try {
    if (!modulePath) throw new Error('no --apptest-module given')
    const url = new URL('./backend-wasm.js', import.meta.url).href
    const backend = await import(url)
    setBackendInfo(await backend.install(registerMethod, modulePath))
  } catch (err) {
    console.error('[testapp] no coredcc backend:', err)
    setBackendInfo({backend: 'none', backendError: String(err)})
  }
}

const canvas = document.getElementById('viewport') as HTMLCanvasElement
const gpu = initGpu(canvas).catch((err) => console.error('WebGPU init failed', err))
// Ready only once the adapter report and the backend are final, so info() is never
// half-initialized.
Promise.allSettled([gpu, loadBackend()]).then(() => {
  installTestbus()
  console.log('[testapp] ready')
})
