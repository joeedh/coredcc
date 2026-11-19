/**
 * Spawns the test app under NW.js and finds its CDP port. Used by the `launch.mjs` CLI
 * and by the test harness.
 *
 * Pressure-tested behaviour this relies on (docs/plans/2026-11-19-test-app.md):
 * - `--remote-debugging-port=0` makes Chromium pick a port and write it to
 *   `<profile>/DevToolsActivePort`, and print `DevTools listening on ws://...` to stderr.
 * - A launch on a profile that is already in use prints `Opening in existing browser
 *   session.` and exits 0, dropping its arguments; that is treated as failure.
 * - `--remote-allow-origins=*` is deliberately NOT passed: it lets any web page that
 *   finds the port drive CDP, and so run Node code.
 * - The child is not `detached`, so it dies with the launching process.
 */
import {spawn} from 'node:child_process'
import fs from 'node:fs'
import {createRequire} from 'node:module'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {deleteProfile, makeThrowawayProfile} from './profile_dir.mjs'

const requireCjs = createRequire(import.meta.url)

/** The NW.js app root: apps/testapp (its package.json is the manifest). */
export const APP_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)))

let nwBinCache
async function nwBin() {
  nwBinCache ??= await requireCjs('nw').findpath()
  return nwBinCache
}

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms))
}

function readActivePort(profileDir) {
  try {
    const text = fs.readFileSync(path.join(profileDir, 'DevToolsActivePort'), 'utf8')
    const port = Number.parseInt(text.split('\n')[0], 10)
    return Number.isFinite(port) && port > 0 ? port : undefined
  } catch {
    return undefined
  }
}

/** True if a running NW.js holds this profile (its lockfile can't be deleted). */
export function profileInUse(profileDir) {
  try {
    fs.rmSync(path.join(profileDir, 'lockfile'), {force: true})
    return false
  } catch (err) {
    return err.code === 'EPERM' || err.code === 'EBUSY'
  }
}

/**
 * @typedef {object} LaunchOptions
 * @property {string} [profileDir]   persistent profile; omitted = fresh throwaway profile
 * @property {boolean} [headless]    keep the window hidden
 * @property {'swiftshader'} [webgpu] force the software WebGPU adapter
 * @property {string[]} [appArgs]    extra app arguments (reach the page as nw.App.argv)
 * @property {string} [artifactsDir] where the log file and NW.js stdio go; default: profile
 * @property {number} [timeoutMs]    how long to wait for the CDP port (default 20 s)
 */

/**
 * @typedef {object} Instance
 * @property {number} pid           the NW.js browser process
 * @property {number} port          CDP port
 * @property {string} profileDir
 * @property {boolean} throwaway
 * @property {string} logFile       the app's line-flushed log file
 * @property {import('node:child_process').ChildProcess} child
 * @property {Promise<{code: number|null, signal: string|null}>} exited
 */

/** Launch the app. Rejects (and cleans up a throwaway profile) if it can't start. */
export async function launch(opts = {}) {
  const throwaway = !opts.profileDir
  const profileDir = opts.profileDir ?? makeThrowawayProfile()
  fs.mkdirSync(profileDir, {recursive: true})
  if (!throwaway && profileInUse(profileDir)) {
    throw new Error(`profile in use by a running instance: ${profileDir}`)
  }
  // It survives a clean exit, so a stale one would name a dead or reused port.
  fs.rmSync(path.join(profileDir, 'DevToolsActivePort'), {force: true})

  const artifactsDir = opts.artifactsDir ?? profileDir
  fs.mkdirSync(artifactsDir, {recursive: true})
  const logFile = path.join(artifactsDir, 'app.log')

  const args = [
    APP_DIR,
    `--user-data-dir=${profileDir}`,
    '--remote-debugging-port=0',
    '--force-device-scale-factor=1',
  ]
  if (opts.webgpu === 'swiftshader') {
    args.push('--use-webgpu-adapter=swiftshader', '--enable-unsafe-webgpu')
  }
  if (opts.headless) args.push('--apptest-headless')
  args.push(`--apptest-log=${logFile}`, ...(opts.appArgs ?? []))

  const child = spawn(await nwBin(), args, {stdio: ['ignore', 'pipe', 'pipe']})
  const stdio = fs.createWriteStream(path.join(artifactsDir, 'nw-stdio.txt'))
  let stderrPort
  let forwarded = false
  let pending = ''
  child.stdout.on('data', (d) => stdio.write(d))
  child.stderr.on('data', (d) => {
    stdio.write(d)
    pending += d.toString()
    const lines = pending.split(/\r?\n/)
    pending = lines.pop()
    for (const line of lines) {
      const m = /DevTools listening on ws:\/\/[^:]+:(\d+)\//.exec(line)
      if (m) stderrPort = Number(m[1])
      if (line.includes('Opening in existing browser session')) forwarded = true
    }
  })
  let exitInfo
  const exited = new Promise((resolve) => {
    child.on('exit', (code, signal) => {
      exitInfo = {code, signal}
      stdio.end()
      resolve(exitInfo)
    })
  })

  const fail = async (message) => {
    if (!exitInfo) child.kill()
    await exited
    if (throwaway) deleteProfile(profileDir)
    throw new Error(message)
  }

  const deadline = Date.now() + (opts.timeoutMs ?? 20000)
  for (;;) {
    if (forwarded) {
      return fail(`profile in use: NW.js forwarded this launch to the running instance (${profileDir})`)
    }
    const port = stderrPort ?? readActivePort(profileDir)
    if (port) {
      return {pid: child.pid, port, profileDir, throwaway, logFile, child, exited}
    }
    if (exitInfo) {
      return fail(`NW.js exited before opening CDP (code ${exitInfo.code}, signal ${exitInfo.signal})`)
    }
    if (Date.now() > deadline) return fail('timed out waiting for the CDP port')
    await sleep(50)
  }
}
