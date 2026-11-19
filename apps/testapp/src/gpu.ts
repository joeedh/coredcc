/**
 * Minimal WebGPU setup for TA0: pick an adapter, report it, and clear the viewport every
 * frame so screenshots show a known colour. The DrawList renderer replaces this in TA3.
 */

export interface AdapterReport {
  available: boolean
  vendor?: string
  architecture?: string
  description?: string
  isFallbackAdapter?: boolean
}

export const CLEAR_COLOR = {r: 0.2, g: 0.4, b: 0.8, a: 1}

let report: AdapterReport = {available: false}

export function adapterReport(): AdapterReport {
  return report
}

export async function initGpu(canvas: HTMLCanvasElement): Promise<void> {
  const gpu = (navigator as any).gpu
  if (!gpu) return
  const adapter = await gpu.requestAdapter()
  if (!adapter) return
  const info = adapter.info ?? {}
  report = {
    available: true,
    vendor: info.vendor,
    architecture: info.architecture,
    description: info.description,
    isFallbackAdapter: info.isFallbackAdapter ?? adapter.isFallbackAdapter ?? false,
  }
  const device = await adapter.requestDevice()
  const ctx = canvas.getContext('webgpu') as any
  const format = gpu.getPreferredCanvasFormat()
  ctx.configure({device, format, alphaMode: 'opaque'})

  const frame = () => {
    canvas.width = canvas.clientWidth
    canvas.height = canvas.clientHeight
    if (canvas.width > 0 && canvas.height > 0) {
      const enc = device.createCommandEncoder()
      const pass = enc.beginRenderPass({
        colorAttachments: [
          {view: ctx.getCurrentTexture().createView(), clearValue: CLEAR_COLOR, loadOp: 'clear', storeOp: 'store'},
        ],
      })
      pass.end()
      device.queue.submit([enc.finish()])
    }
    requestAnimationFrame(frame)
  }
  requestAnimationFrame(frame)
}
