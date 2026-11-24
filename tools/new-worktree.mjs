#!/usr/bin/env node
// Creates a worktree beside the main checkout, ready to configure.
//
//   node tools/new-worktree.mjs <branch> [--base <ref>] [--deps]
//
//   --base   where a new branch starts (default: master)
//   --deps   also install the node dependencies of tests/wasm and apps/testapp
//
// Run it again if it fails after the worktree exists: it picks up where it stopped. It
// doesn't remove worktrees; run `node tools/cache.mjs setup` after removing one.

import {execFileSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {parseArgs} from 'node:util'
import {checkouts, git, mainCheckout} from './checkout.mjs'

const MAX_WORKTREES = 3

function fail(message) {
  console.error(`new-worktree: ${message}`)
  process.exit(1)
}

const show = (file, args, options) => execFileSync(file, args, {stdio: 'inherit', ...options})

const {values, positionals} = parseArgs({
  allowPositionals: true,
  options: {base: {type: 'string', default: 'master'}, deps: {type: 'boolean', default: false}},
})
const branch = positionals[0]
if (positionals.length !== 1 || !/^[A-Za-z0-9._/][A-Za-z0-9._/-]*$/.test(branch)) {
  fail('usage: new-worktree.mjs <branch> [--base <ref>] [--deps]; a branch is [A-Za-z0-9._/-]')
}
const main = mainCheckout()
if (!main) {
  fail("this checkout isn't a repository of its own")
}
const folder = path.join(
  path.dirname(main),
  `${path.basename(main)}-${branch.replaceAll('/', '-')}`
)

git(main, 'worktree', 'prune')
const existing = checkouts(main)
const samePath = (a, b) => a.toLowerCase() === b.toLowerCase()
const here = existing.find((c) => samePath(c.folder, folder))
if (here) {
  if (here.branch !== branch) {
    fail(`${folder} is a worktree of ${here.branch ?? 'no branch'}`)
  }
  console.log(`new-worktree: ${folder} exists, completing it`)
} else {
  if (fs.existsSync(folder)) {
    fail(`${folder} exists and isn't a worktree`)
  }
  if (existing.length > MAX_WORKTREES) {
    fail(`there are ${MAX_WORKTREES} worktrees already`)
  }
  let known = true
  try {
    git(main, 'rev-parse', '--verify', '--quiet', `refs/heads/${branch}`)
  } catch {
    known = false
  }
  const what = known ? [folder, branch] : ['-b', branch, folder, values.base]
  show('git', ['-C', main, 'worktree', 'add', ...what])
}

// --dissociate: the worktree must survive whatever happens to the main submodule.
const litestl = path.join(main, 'extern', 'litestl')
const reference = fs.existsSync(path.join(litestl, '.git'))
  ? ['--reference', litestl, '--dissociate']
  : []
show('git', ['-C', folder, 'submodule', 'update', '--init', ...reference])

// The new checkout's copy may predate the tool.
show(process.execPath, [path.join(import.meta.dirname, 'cache.mjs'), 'setup'])

if (values.deps) {
  const shell = process.platform === 'win32'
  show('npm', ['ci'], {cwd: path.join(folder, 'tests', 'wasm'), shell})
  show('pnpm', ['install', '--frozen-lockfile'], {
    cwd: path.join(folder, 'apps', 'testapp'),
    shell,
  })
}

const jobs = Math.max(2, Math.floor(os.availableParallelism() / checkouts(main).length))
console.log(`
new-worktree: ${folder} is ready
  cd ${folder}
  tools\\devshell.cmd cmake --preset native-clang
  tools\\devshell.cmd cmake --build --preset native-clang-debug -j ${jobs}`)
if (!values.deps) {
  console.log('  without --deps, the ctests spike_ts and testapp skip')
}
