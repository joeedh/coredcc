/* Writes a module's generated TypeScript to <outdir>, with the generator linked into
 * the module: node gen.mjs <module.mjs> <outdir> <getManager export>. The module must be
 * a node build. Uses litestl's runtime only: coredcc's TS layer imports what this
 * writes. */
import {mkdirSync, writeFileSync} from 'node:fs'
import {dirname, join} from 'node:path'
import {pathToFileURL} from 'node:url'

import {BindingManager, createWasmHelpers} from '@litestl/typescript-runtime'

const [modulePath, outDir, getManager] = process.argv.slice(2)
if (!getManager) throw new Error('usage: gen.mjs <module.mjs> <outdir> <getManager>')
const create = (await import(pathToFileURL(modulePath).href)).default
const wasm = createWasmHelpers(await create(), undefined) as any
const manager = new BindingManager(wasm, wasm[getManager]())
manager.load()
for (const [name, body] of manager.generateTypeScript()) {
  const file = join(outDir, name)
  mkdirSync(dirname(file), {recursive: true})
  writeFileSync(file, body)
}
