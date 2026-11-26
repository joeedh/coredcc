/**
 * The test app's DrawList renderer (TA3): the tessellated batches (tessellate.ts) drawn with
 * one WebGPU shader and three pipelines, by depth mode, over a depth buffer.
 *
 * Order: Test, then X-ray tested, then X-ray where hidden at a third of its alpha, then Always.
 */
import type {DecodedDrawList} from 'coredcc-script'
import {FLOATS_PER_VERTEX, tessellate, type Viewport} from './tessellate.ts'

/** What the page's gpu.ts hands the backend. */
export interface GpuHooks {
  gpuState(): {device: any; context: any; format: string; canvas: HTMLCanvasElement} | undefined
  setRenderer(r: ((gpu: any, encoder: any, target: any) => void) | undefined): void
  renderNow(): Promise<string | undefined>
  readPixel(x: number, y: number): Promise<number[] | undefined>
}

export const BACKGROUND = {r: 0.16, g: 0.16, b: 0.18, a: 1}

const SHADER = /* wgsl */ `
struct Out {
  @builtin(position) pos: vec4f,
  @location(0) color: vec4f,
}
struct Uniforms { alpha: vec4f }
@group(0) @binding(0) var<uniform> u: Uniforms;
@vertex fn vs(@location(0) pos: vec4f, @location(1) color: vec4f) -> Out {
  var o: Out;
  o.pos = pos;
  o.color = vec4f(color.rgb, color.a * u.alpha.x);
  return o;
}
@fragment fn fs(i: Out) -> @location(0) vec4f {
  return i.color;
}
`

type Mode = 'test' | 'always' | 'hidden'

export class OverlayRenderer {
  private pipelines = new Map<Mode, any>()
  private depth: any
  private depthSize = ''
  private full: any
  private third: any
  /* The last frame's vertex buffers: destroyed once the next one starts (WebGPU waits). */
  private transient: any[] = []

  constructor(
    private device: any,
    format: string
  ) {
    const module = device.createShaderModule({code: SHADER})
    const layout = device.createBindGroupLayout({
      entries: [{binding: 0, visibility: 1 /* VERTEX */, buffer: {type: 'uniform'}}],
    })
    const pipelineLayout = device.createPipelineLayout({bindGroupLayouts: [layout]})
    const make = (depthCompare: string, depthWriteEnabled: boolean) =>
      device.createRenderPipeline({
        layout: pipelineLayout,
        vertex: {
          module,
          entryPoint: 'vs',
          buffers: [
            {
              arrayStride: FLOATS_PER_VERTEX * 4,
              attributes: [
                {shaderLocation: 0, offset: 0, format: 'float32x4'},
                {shaderLocation: 1, offset: 16, format: 'float32x4'},
              ],
            },
          ],
        },
        fragment: {
          module,
          entryPoint: 'fs',
          targets: [
            {
              format,
              blend: {
                color: {srcFactor: 'src-alpha', dstFactor: 'one-minus-src-alpha', operation: 'add'},
                alpha: {srcFactor: 'one', dstFactor: 'one-minus-src-alpha', operation: 'add'},
              },
            },
          ],
        },
        primitive: {topology: 'triangle-list', cullMode: 'none'},
        depthStencil: {format: 'depth24plus', depthCompare, depthWriteEnabled},
      })
    this.pipelines.set('test', make('less-equal', true))
    this.pipelines.set('always', make('always', false))
    this.pipelines.set('hidden', make('greater', false))
    const uniform = (alpha: number) => {
      const buf = device.createBuffer({size: 16, usage: 0x40 /* UNIFORM */ | 0x8 /* COPY_DST */})
      device.queue.writeBuffer(buf, 0, new Float32Array([alpha, 0, 0, 0]))
      return device.createBindGroup({layout, entries: [{binding: 0, resource: {buffer: buf}}]})
    }
    this.full = uniform(1)
    this.third = uniform(1 / 3)
  }

  /** Clears `target` and draws `list` over it. */
  render(encoder: any, target: any, list: DecodedDrawList | undefined, viewProj: ArrayLike<number>, vp: Viewport) {
    for (const b of this.transient) b.destroy()
    this.transient = []
    const size = `${target.width}x${target.height}`
    if (size !== this.depthSize) {
      this.depth?.destroy()
      this.depth = this.device.createTexture({
        size: [target.width, target.height],
        format: 'depth24plus',
        usage: 0x10 /* RENDER_ATTACHMENT */,
      })
      this.depthSize = size
    }
    const pass = encoder.beginRenderPass({
      colorAttachments: [{view: target.createView(), clearValue: BACKGROUND, loadOp: 'clear', storeOp: 'store'}],
      depthStencilAttachment: {view: this.depth.createView(), depthClearValue: 1, depthLoadOp: 'clear', depthStoreOp: 'store'},
    })
    if (list) {
      const b = tessellate(list, viewProj, vp)
      const draws: [Mode, number[], any][] = [
        ['test', b.test, this.full],
        ['test', b.xray, this.full],
        ['hidden', b.xray, this.third],
        ['always', b.always, this.full],
      ]
      for (const [mode, data, group] of draws) {
        if (data.length === 0) continue
        const bytes = new Float32Array(data)
        const buf = this.device.createBuffer({size: bytes.byteLength, usage: 0x20 /* VERTEX */ | 0x8 /* COPY_DST */})
        this.transient.push(buf)
        this.device.queue.writeBuffer(buf, 0, bytes)
        pass.setPipeline(this.pipelines.get(mode))
        pass.setBindGroup(0, group)
        pass.setVertexBuffer(0, buf)
        pass.draw(bytes.length / FLOATS_PER_VERTEX)
      }
    }
    pass.end()
  }
}
