#!/usr/bin/env node
// Installs the pinned Emscripten SDK into build/deps/emsdk (gitignored), the main
// checkout's if this is a worktree.
//
//   node tools/install-emsdk.mjs              clone, install and activate
//   node tools/install-emsdk.mjs --adopt DIR  move an existing emsdk checkout there,
//                                            after checking its commit and version
//   node tools/install-emsdk.mjs --check      report whether the pinned SDK is there
//
// The pin matches sculptcore's (emsdkVersion.txt and EMSDK_COMMIT in its make.mjs): one
// WASM module needs one toolchain. Activation is local to the checkout (no --permanent).

import {execFileSync} from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import {mainCheckout, root} from './checkout.mjs'

const pin = JSON.parse(fs.readFileSync(path.join(root, 'tools', 'emsdk.json'), 'utf8'))
const target = path.join(mainCheckout() ?? root, 'build', 'deps', 'emsdk')

function emscriptenVersion(dir) {
  const f = path.join(dir, 'upstream', 'emscripten', 'emscripten-version.txt')
  return fs.existsSync(f) ? fs.readFileSync(f, 'utf8').trim().replace(/"/g, '') : null
}

function commitOf(dir) {
  try {
    return execFileSync('git', ['-C', dir, 'rev-parse', 'HEAD'], {encoding: 'utf8'}).trim()
  } catch {
    return null
  }
}

function problems(dir) {
  const out = []
  if (!fs.existsSync(dir)) {
    return ['missing']
  }
  const commit = commitOf(dir)
  if (!commit || !commit.startsWith(pin.commit)) {
    out.push(`emsdk commit is ${commit ?? 'unknown'}, want ${pin.commit}`)
  }
  const version = emscriptenVersion(dir)
  if (version !== pin.version) {
    out.push(`emscripten is ${version ?? 'not installed'}, want ${pin.version}`)
  }
  if (!fs.existsSync(path.join(dir, '.emscripten'))) {
    out.push('not activated (no .emscripten)')
  }
  return out
}

function emsdk(args) {
  const script = process.platform === 'win32' ? 'emsdk.bat' : './emsdk'
  execFileSync(script, args, {cwd: target, stdio: 'inherit', shell: process.platform === 'win32'})
}

const args = process.argv.slice(2)
if (args[0] === '--check') {
  const p = problems(target)
  console.log(p.length ? `emsdk: ${p.join('; ')}` : `emsdk ${pin.version} ready at ${target}`)
  process.exit(p.length ? 1 : 0)
}

if (problems(target).length === 0) {
  console.log(`emsdk ${pin.version} is already installed at ${target}`)
  process.exit(0)
}
if (fs.existsSync(target)) {
  console.error(`${target} exists but isn't the pinned SDK: ${problems(target).join('; ')}`)
  console.error('Remove it and run this again.')
  process.exit(1)
}
fs.mkdirSync(path.dirname(target), {recursive: true})

if (args[0] === '--adopt') {
  const from = path.resolve(args[1] ?? '')
  const p = problems(from)
  if (p.length) {
    console.error(`can't adopt ${from}: ${p.join('; ')}`)
    process.exit(1)
  }
  try {
    fs.renameSync(from, target) // same volume: a move, not a copy
  } catch (e) {
    /* Seen from sandboxed shells: the move is refused although nothing holds the files. */
    console.error(`moving ${from} failed (${e.code}); move it to ${target} by hand`)
    process.exit(1)
  }
  console.log(`moved ${from} to ${target}`)
  process.exit(0)
}

execFileSync('git', ['clone', 'https://github.com/emscripten-core/emsdk.git', target], {
  stdio: 'inherit',
})
execFileSync('git', ['-C', target, 'checkout', pin.commit], {stdio: 'inherit'})
emsdk(['install', pin.version])
emsdk(['activate', pin.version])
const p = problems(target)
if (p.length) {
  console.error(`install finished, but: ${p.join('; ')}`)
  process.exit(1)
}
console.log(`emsdk ${pin.version} installed at ${target}`)
