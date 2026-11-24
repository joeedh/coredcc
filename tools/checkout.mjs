// Which checkout a tool runs in, and where the checkouts' shared build cache is.
// cmake/CoredccCache.cmake derives the same folder; keep the two in step.

import {execFileSync} from 'node:child_process'
import fs from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'

/** The checkout this file is in. */
export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..')

export function git(dir, ...args) {
  return execFileSync('git', ['-C', dir, ...args], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
  }).trim()
}

/** Forward slashes, as CMake and sccache want them. */
export const slashes = (p) => p.replaceAll('\\', '/')

/**
 * The main checkout of the repository `dir` is in, or null if there is none: not a git
 * checkout, a bare repository, or coredcc as another project's submodule.
 */
export function mainCheckout(dir = root) {
  let common
  try {
    common = git(dir, 'rev-parse', '--path-format=absolute', '--git-common-dir')
  } catch {
    return null
  }
  return path.basename(common) === '.git' ? path.resolve(path.dirname(common)) : null
}

/** The existing checkouts of the repository `dir` is in, the main one first. */
export function checkouts(dir = root) {
  const out = []
  for (const entry of git(dir, 'worktree', 'list', '--porcelain').split(/\r?\n\r?\n/)) {
    const folder = /^worktree (.+)$/m.exec(entry)?.[1]
    if (folder && fs.existsSync(folder)) {
      const branch = /^branch refs\/heads\/(.+)$/m.exec(entry)?.[1] ?? null
      out.push({folder: path.resolve(folder), branch})
    }
  }
  return out
}

/** The folder of the build cache, or null if this checkout can't have one. */
export function cacheFolder(dir = root) {
  if (process.env.COREDCC_CACHE_DIR) {
    return path.resolve(process.env.COREDCC_CACHE_DIR)
  }
  const main = mainCheckout(dir)
  return main && path.join(path.dirname(main), `.${path.basename(main)}-cache`)
}
