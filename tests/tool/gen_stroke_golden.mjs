// Writes stroke_golden.inc: strokes in screen space and the samples that sculptcore's own
// brush/stroke_driver.cc makes of them, for test_tool_stroke.
//   node tests/tool/gen_stroke_golden.mjs [--sculptcore <dir>]   (default: ../sculptcore)
// It builds stroke_driver.cc with clang-cl (through tools/devshell.cmd) against sculptcore's
// own litestl, with a stub spatial/spatial.h whose castRay misses: the real one pulls in the
// mesh, GPU and N-API headers, and screen space needs no geometry. Inputs are multiples of
// 1/64, so the driver's float arguments hold them exactly.
import { execFileSync } from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const repo = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const argv = process.argv.slice(2)
const at = argv.indexOf('--sculptcore')
const sculptcore = path.resolve(at >= 0 ? argv[at + 1] : path.join(repo, '..', 'sculptcore'))
const src = path.join(sculptcore, 'source')
if (!fs.existsSync(path.join(src, 'brush', 'stroke_driver.cc'))) {
  console.error(`no sculptcore at ${sculptcore} (give --sculptcore <dir>)`)
  process.exit(1)
}
const out = path.join(repo, 'build', 'stroke-golden')
fs.mkdirSync(path.join(out, 'spatial'), { recursive: true })

let seed = 20261126
const random = () => { seed = (Math.imul(seed, 1664525) + 1013904223) >>> 0; return seed / 4294967296 }
const q = (v) => Math.round(v * 64) / 64

// Each stroke: its radius and spacing, and its pointer samples in batches (one poll each).
const strokes = []
const add = (radius, spacing, points, perBatch) => {
  const batches = []
  for (let i = 0; i < points.length; i += perBatch) batches.push(points.slice(i, i + perBatch))
  strokes.push({ radius, spacing, batches })
}
add(20, 0.25, Array.from({ length: 16 }, (_, i) => [q(100 + i * 9), q(200), q(0.5)]), 1)
add(15, 0.1, Array.from({ length: 30 }, (_, i) => {
  const a = i * 0.12
  return [q(400 + 150 * Math.cos(a) + random() * 2), q(300 + 150 * Math.sin(a) + random() * 2), q(0.2 + i / 40)]
}), 3)
add(10, 0.5, [[100, 100, 0.5], [100, 100, 0.5], [160, 100, 0.75], [160, 100, 0.75], [160, 160, 1], [100, 170, 0.25]], 2)
add(30, 0.05, Array.from({ length: 40 }, (_, i) => [q(300 + i * 0.5), q(300 + Math.sin(i / 5) * 4), q(0.5 + 0.4 * Math.sin(i / 7))]), 5)
add(12, 0.2, [[50, 50, 0.5]], 1)

fs.writeFileSync(path.join(out, 'spatial', 'spatial.h'), `#pragma once
/* A stub for gen_stroke_golden.mjs: nothing is hit. */
#include "spatial/spatial_base.h"
namespace sculptcore::spatial {
struct SpatialTree {
  bool castRay(litestl::math::float3, litestl::math::float3, CastRayIsect &) { return false; }
};
} // namespace sculptcore::spatial
`)

const inputs = strokes.flatMap((s, k) => s.batches.flatMap((b, j) => b.map(([x, y, p]) => `{${k}, ${j}, ${x}, ${y}, ${p}}`)))
fs.writeFileSync(path.join(out, 'main.cc'), `#include "brush/stroke_driver.h"
#include "spatial/spatial.h"
#include <cstdio>
using namespace sculptcore;
struct In { int stroke, batch; float x, y, pressure; };
static const In kIn[] = {${inputs.join(', ')}};
static const float kRadius[] = {${strokes.map((s) => s.radius).join(', ')}};
static const float kSpacing[] = {${strokes.map((s) => s.spacing).join(', ')}};
static const int kBatches[] = {${strokes.map((s) => s.batches.length).join(', ')}};
int main() {
  std::printf("// Written by tests/tool/gen_stroke_golden.mjs from sculptcore's brush/stroke_driver.cc.\\n");
  std::printf("// Do not edit. {stroke, batch (the last is the poll after end), x, y, pressure, angle, interpolated}\\n");
  std::printf("static const GoldenInput kGoldenInputs[] = {\\n");
  for (const In &i : kIn) std::printf("{%d, %d, %.9g, %.9g, %.9g},\\n", i.stroke, i.batch, i.x, i.y, i.pressure);
  std::printf("};\\nstatic const GoldenStroke kGoldenStrokes[] = {\\n");
  for (int s = 0; s < int(sizeof(kRadius) / sizeof(float)); s++)
    std::printf("{%.9g, %.9g, %d},\\n", kRadius[s], kSpacing[s], kBatches[s]);
  std::printf("};\\nstatic const GoldenSample kGoldenSamples[] = {\\n");
  spatial::SpatialTree tree;
  for (int s = 0; s < int(sizeof(kRadius) / sizeof(float)); s++) {
    brush::BrushStrokeDriver d(&tree);
    for (int r = 0; r < 4; r++) d.setViewRow(0, r, r == 0, r == 1, r == 2, r == 3);
    d.setViewParams(0, 0, 0, 800, 600, 800, 600, 0.01f, false);
    for (int b = 0; b <= kBatches[s]; b++) {
      for (const In &i : kIn)
        if (i.stroke == s && i.batch == b)
          d.push(i.x, i.y, i.pressure, 0, 0, 0, false, false, kRadius[s], 1, kSpacing[s]);
      if (b == kBatches[s]) d.end();
      const int n = d.poll();
      for (int k = 0; k < n; k++) {
        const brush::DabSample *p = d.sampleAt(k);
        std::printf("{%d, %d, %.9g, %.9g, %.9g, %.9g, %d},\\n", s, b, p->screenP[0], p->screenP[1],
                    p->pressure, p->angle, p->isInterp ? 1 : 0);
      }
    }
  }
  std::printf("};\\n");
  return 0;
}
`)

const lt = path.join(src, 'litestl')
const sources = [path.join(out, 'main.cc'), path.join(src, 'brush', 'stroke_driver.cc'),
  ...['util/alloc.cc', 'util/string.cc', 'util/util.cc', 'platform/common.cc', 'platform/win32.cc'].map((f) => path.join(lt, f))]
const includes = [out, src, lt, path.join(lt, 'extern')].map((d) => `/I${d}`)
const exe = path.join(out, 'stroke_golden.exe')
const args = ['/nologo', '/std:c++20', '/EHsc', '/O2', '/w', '/DNOMINMAX', ...includes, ...sources, `/Fe${exe}`, `/Fo${out}/`, 'dbghelp.lib']
execFileSync('cmd.exe', ['/d', '/s', '/c', `""${path.join(repo, 'tools', 'devshell.cmd')}" clang-cl ${args.map((a) => `"${a}"`).join(' ')}"`],
  { stdio: ['ignore', 'inherit', 'inherit'], windowsVerbatimArguments: true })
const text = execFileSync(exe, { encoding: 'utf8' })
fs.writeFileSync(path.join(repo, 'tests', 'tool', 'stroke_golden.inc'), text.replace(/\r\n/g, '\n'))
console.log(`wrote tests/tool/stroke_golden.inc: ${text.split('\n').length} lines`)
