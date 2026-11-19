/**
 * Test harness: launch the app on a throwaway profile, connect over CDP, record logs,
 * and tear down. On failure (or a crash) the profile's crash dumps and the logs are
 * copied to `apps/testapp/test-artifacts/<name>/` before the profile is deleted.
 *
 * Tests import values only from `.mjs` modules here; page modules are `import type` only
 * (Node's type stripping can't load them).
 */
import fs from 'node:fs'
import path from 'node:path'
import {CdpClient} from '../cdp.mjs'
import {APP_DIR, launch} from '../nwjs/launcher.mjs'
import {crashReportsDir, deleteProfile, gcThrowawayProfiles} from '../nwjs/profile_dir.mjs'

export const ARTIFACTS_ROOT = path.join(APP_DIR, 'test-artifacts')

export interface AppOptions {
  name: string
  headless?: boolean
  webgpu?: 'swiftshader'
  appArgs?: string[]
}

export interface App {
  client: CdpClient
  port: number
  pid: number
  profileDir: string
  artifactsDir: string
  logFile: string
  /** True once the CDP socket closed without close() being called. */
  crashed(): boolean
  exited: Promise<{code: number | null; signal: string | null}>
  /** Quit, wait for exit, keep artifacts if `failed` or crashed, delete the profile. */
  close(opts?: {failed?: boolean}): Promise<void>
}

let gcDone = false

export async function startApp(opts: AppOptions): Promise<App> {
  if (!gcDone) {
    gcThrowawayProfiles()
    gcDone = true
  }
  const artifactsDir = path.join(ARTIFACTS_ROOT, `${opts.name}-${process.pid}-${Date.now().toString(36)}`)
  const inst = await launch({
    headless: opts.headless ?? true,
    webgpu: opts.webgpu,
    appArgs: opts.appArgs,
    artifactsDir,
  })
  let closing = false
  let crashed = false
  let client: CdpClient
  try {
    client = await CdpClient.connect(inst.port)
    client.on('$close', () => {
      if (!closing) crashed = true
    })
    await client.recordLogs()
    await client.waitReady()
  } catch (err) {
    inst.child.kill()
    await inst.exited
    deleteProfile(inst.profileDir)
    throw err
  }

  const close = async ({failed = false}: {failed?: boolean} = {}) => {
    closing = true
    if (!client.closed) await client.call('quit', [], {timeoutMs: 3000})
    client.close()
    const timer = setTimeout(() => inst.child.kill(), 5000)
    await inst.exited
    clearTimeout(timer)
    const keep = failed || crashed
    if (keep) {
      fs.mkdirSync(artifactsDir, {recursive: true})
      fs.writeFileSync(
        path.join(artifactsDir, 'cdp-log.json'),
        JSON.stringify(client.logs, null, 2),
      )
      const dumps = crashReportsDir(inst.profileDir)
      if (fs.existsSync(dumps)) {
        fs.cpSync(dumps, path.join(artifactsDir, 'crash-reports'), {recursive: true})
      }
    }
    deleteProfile(inst.profileDir)
    if (!keep) fs.rmSync(artifactsDir, {recursive: true, force: true})
  }

  return {
    client,
    port: inst.port,
    pid: inst.pid!,
    profileDir: inst.profileDir,
    artifactsDir,
    logFile: inst.logFile,
    crashed: () => crashed,
    exited: inst.exited,
    close,
  }
}

/** Unwrap a testbus reply, throwing with the remote error if it failed. */
export function ok<T = any>(reply: {ok: boolean; value?: unknown; error?: any}): T {
  if (!reply.ok) throw new Error(`testbus call failed: ${reply.error?.name}: ${reply.error?.message}`)
  return reply.value as T
}
