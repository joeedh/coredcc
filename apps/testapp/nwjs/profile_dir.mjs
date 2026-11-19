/**
 * Chromium profile directories (`--user-data-dir`) for the test app.
 *
 * - Manual launches: one persistent profile per worktree,
 *   `%LOCALAPPDATA%\coredcc\testapp\<dir>-<hash8>\<default|inst-NAME>`.
 * - Test and ephemeral launches: a fresh `os.tmpdir()/coredcc-testapp-XXXXXX`, deleted
 *   after exit and garbage-collected if a runner died before it could delete it.
 *
 * A live NW.js holds `<profile>/lockfile` open, so deleting it fails with EPERM. GC uses
 * that as its liveness test and never recursively deletes a profile whose lockfile it
 * could not remove first (that would gut a running instance's profile).
 */
import crypto from 'node:crypto'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

export const THROWAWAY_PREFIX = 'coredcc-testapp-'

/** Persistent per-worktree profile base. */
export function worktreeProfileBase(worktreeRoot) {
  const root = path.resolve(worktreeRoot)
  const hash = crypto.createHash('sha256').update(root.toLowerCase()).digest('hex').slice(0, 8)
  const local = process.env.LOCALAPPDATA || path.join(os.homedir(), '.local', 'share')
  return path.join(local, 'coredcc', 'testapp', `${path.basename(root)}-${hash}`)
}

/** Persistent profile for a worktree; `instance` selects a named sub-profile. */
export function worktreeProfileDir(worktreeRoot, instance) {
  return path.join(worktreeProfileBase(worktreeRoot), instance ? `inst-${instance}` : 'default')
}

/** Create a fresh throwaway profile directory. */
export function makeThrowawayProfile() {
  return fs.mkdtempSync(path.join(os.tmpdir(), THROWAWAY_PREFIX))
}

/** Where Crashpad writes minidumps under a profile. */
export function crashReportsDir(profileDir) {
  return path.join(profileDir, 'Crashpad', 'reports')
}

function sleepSync(ms) {
  Atomics.wait(new Int32Array(new SharedArrayBuffer(4)), 0, 0, ms)
}

/**
 * Delete a profile that is known (or believed) dead. Removes `lockfile` first; if that
 * fails with EPERM/EBUSY the profile is live and nothing else is touched. Retries for
 * `retryMs` because Windows can hold handles briefly after a process exits.
 * Returns true if the directory is gone.
 */
export function deleteProfile(profileDir, retryMs = 3000) {
  const deadline = Date.now() + retryMs
  for (;;) {
    try {
      fs.rmSync(path.join(profileDir, 'lockfile'), {force: true})
      fs.rmSync(profileDir, {recursive: true, force: true})
      return !fs.existsSync(profileDir)
    } catch (err) {
      if (Date.now() >= deadline) return false
      if (err.code !== 'EPERM' && err.code !== 'EBUSY' && err.code !== 'ENOTEMPTY') return false
      sleepSync(100)
    }
  }
}

/**
 * Remove leaked throwaway profiles. Only touches `coredcc-testapp-*` directories older
 * than `minAgeMs` whose lockfile can be deleted (i.e. whose NW.js is gone).
 * Returns the list of removed directories.
 */
export function gcThrowawayProfiles(minAgeMs = 10 * 60 * 1000) {
  const removed = []
  const tmp = os.tmpdir()
  let names
  try {
    names = fs.readdirSync(tmp)
  } catch {
    return removed
  }
  const cutoff = Date.now() - minAgeMs
  for (const name of names) {
    if (!name.startsWith(THROWAWAY_PREFIX)) continue
    const dir = path.join(tmp, name)
    try {
      if (fs.statSync(dir).mtimeMs > cutoff) continue
      fs.rmSync(path.join(dir, 'lockfile'), {force: true})
    } catch {
      continue // live (EPERM) or vanished
    }
    if (deleteProfile(dir, 0)) removed.push(dir)
  }
  return removed
}
