import {gpuState, initGpu, readPixel, renderNow, setRenderer} from './gpu.ts'
import {installLogMirror} from './log.ts'
import {argValue, hasArg, isNw} from './node.ts'
import {installTestbus, registerMethod, setBackendInfo} from './testbus.ts'

installLogMirror()

if (isNw && !hasArg('apptest-headless')) {
  const win = (globalThis as any).nw.Window.get()
  win.show()
  if (hasArg('devtools')) win.showDevTools()
}

/*
 * The wasm backend is its own bundle (src/backend/wasm.ts). The launcher passes either
 * --apptest-module, or --apptest-no-backend when the build has none (then TA1's tests
 * skip); any other failure to load is reported, and TA1's tests fail.
 */
async function loadBackend(): Promise<void> {
  const missing = argValue('apptest-no-backend')
  if (missing !== undefined) {
    setBackendInfo({backend: 'none', backendMissing: true, backendError: missing})
    return
  }
  try {
    const modulePath = argValue('apptest-module')
    if (!modulePath) throw new Error('no --apptest-module given (launch through nwjs/launcher.mjs)')
    const url = new URL('./backend-wasm.js', import.meta.url).href
    const backend = await import(url)
    setBackendInfo(await backend.install(registerMethod, modulePath, setBackendInfo, {gpuState, setRenderer, renderNow, readPixel}))
  } catch (err) {
    console.error('[testapp] the coredcc backend failed to load:', err)
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
