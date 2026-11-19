import {initGpu} from './gpu.ts'
import {installLogMirror} from './log.ts'
import {hasArg, isNw} from './node.ts'
import {installTestbus} from './testbus.ts'

installLogMirror()

if (isNw && !hasArg('apptest-headless')) {
  const win = (globalThis as any).nw.Window.get()
  win.show()
  if (hasArg('devtools')) win.showDevTools()
}

const canvas = document.getElementById('viewport') as HTMLCanvasElement
initGpu(canvas)
  .catch((err) => console.error('WebGPU init failed', err))
  .finally(() => {
    // Ready only once the adapter report is final, so info() is never half-initialized.
    installTestbus()
    console.log('[testapp] ready')
  })
