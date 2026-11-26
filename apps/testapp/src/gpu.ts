/**
 * WebGPU setup: pick an adapter, report it, and render the viewport every frame. Until the
 * backend sets a renderer (TA3's overlay, src/overlay/), a frame is the clear color, so
 * screenshots show a known color.
 */

export interface AdapterReport {
  available: boolean
  vendor?: string
  architecture?: string
  description?: string
  isFallbackAdapter?: boolean
}

export const CLEAR_COLOR = {r: 0.2, g: 0.4, b: 0.8, a: 1}

/** What a renderer draws with. */
export interface Gpu {
  device: any
  context: any
  format: string
  canvas: HTMLCanvasElement
}

/** Draws one frame into the canvas's current texture: it clears it itself. */
export type Renderer = (gpu: Gpu, encoder: any, target: any) => void

let report: AdapterReport = {available: false}
let gpu: Gpu | undefined
let renderer: Renderer | undefined

export function adapterReport(): AdapterReport {
  return report
}

/** The device and the canvas; undefined without an adapter. */
export function gpuState(): Gpu | undefined {
  return gpu
}

export function setRenderer(r: Renderer | undefined) {
  renderer = r
}

/** Renders a frame now; resolves once the GPU has done it, with WebGPU's validation error, if
 * any. Without an adapter, at once. */
export async function renderNow(): Promise<string | undefined> {
  if (!gpu) return undefined
  gpu.device.pushErrorScope('validation')
  draw(gpu)
  const error = await gpu.device.popErrorScope()
  await gpu.device.queue.onSubmittedWorkDone()
  return error ? String(error.message) : undefined
}

/**
 * Renders a frame and reads back the RGBA of canvas pixel (x, y), from the GPU. A hidden window
 * doesn't composite frames drawn outside rAF, so shot() can't see them; this can.
 */
export async function readPixel(x: number, y: number): Promise<number[] | undefined> {
  if (!gpu) return undefined
  draw(gpu)
  const t = gpu.context.getCurrentTexture()
  const buf = gpu.device.createBuffer({size: 256, usage: 0x8 | 0x1 /* COPY_DST | MAP_READ */})
  try {
    const enc = gpu.device.createCommandEncoder()
    const origin = [Math.min(t.width - 1, Math.max(0, Math.floor(x))), Math.min(t.height - 1, Math.max(0, Math.floor(y)))]
    enc.copyTextureToBuffer({texture: t, origin}, {buffer: buf, bytesPerRow: 256}, [1, 1])
    gpu.device.queue.submit([enc.finish()])
    await buf.mapAsync(1 /* READ */)
    const [a, b, c, d] = Array.from(new Uint8Array(buf.getMappedRange()).slice(0, 4))
    buf.unmap()
    return gpu.format.startsWith('bgra') ? [c, b, a, d] : [a, b, c, d]
  } finally {
    buf.destroy()
  }
}

function draw(g: Gpu) {
  const {canvas, device, context} = g
  canvas.width = canvas.clientWidth
  canvas.height = canvas.clientHeight
  if (canvas.width <= 0 || canvas.height <= 0) return
  const enc = device.createCommandEncoder()
  const target = context.getCurrentTexture()
  if (renderer) {
    renderer(g, enc, target)
  } else {
    const pass = enc.beginRenderPass({
      colorAttachments: [{view: target.createView(), clearValue: CLEAR_COLOR, loadOp: 'clear', storeOp: 'store'}],
    })
    pass.end()
  }
  device.queue.submit([enc.finish()])
}

export async function initGpu(canvas: HTMLCanvasElement): Promise<void> {
  const nav = (navigator as any).gpu
  if (!nav) return
  const adapter = await nav.requestAdapter()
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
  const context = canvas.getContext('webgpu') as any
  const format = nav.getPreferredCanvasFormat()
  context.configure({device, format, alphaMode: 'opaque', usage: 0x10 | 0x01 /* RENDER_ATTACHMENT | COPY_SRC, for readPixel */})
  gpu = {device, context, format, canvas}

  const frame = () => {
    draw(gpu!)
    requestAnimationFrame(frame)
  }
  requestAnimationFrame(frame)
}
