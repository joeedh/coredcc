#!/usr/bin/env node
/**
 * Manual launcher for the test app.
 *
 *   node apps/testapp/nwjs/launch.mjs [--headless] [--instance[=NAME] | --ephemeral]
 *                                     [--webgpu=swiftshader] [app args...]
 *
 * - Default: this worktree's persistent profile. `--instance=NAME` uses a named
 *   sub-profile; a bare `--instance` or `--ephemeral` uses a throwaway one, deleted on exit.
 * - Prints the profile, PID and CDP port, then stays attached until the app exits
 *   (the app dies with this process). Drive it with `node apps/testapp/cdp.mjs`.
 * - Unknown `--` switches pass through to the page as app arguments.
 */
import path from 'node:path'
import {launch, APP_DIR} from './launcher.mjs'
import {deleteProfile, gcThrowawayProfiles, worktreeProfileDir} from './profile_dir.mjs'

const repoRoot = path.resolve(APP_DIR, '..', '..')
const opts = {appArgs: []}
let instance
let throwaway = false

for (const arg of process.argv.slice(2)) {
  const inst = /^--instance(?:=(.+))?$/.exec(arg)
  if (inst) {
    if (inst[1]) instance = inst[1]
    else throwaway = true
  } else if (arg === '--ephemeral') throwaway = true
  else if (arg === '--headless') opts.headless = true
  else if (arg === '--webgpu=swiftshader') opts.webgpu = 'swiftshader'
  else if (arg === '--headless=new' || arg.startsWith('--remote-')) {
    console.error(`[testapp] refusing ${arg}: it breaks NW.js or exposes CDP to web pages`)
    process.exit(2)
  } else opts.appArgs.push(arg)
}
if (!throwaway) opts.profileDir = worktreeProfileDir(repoRoot, instance)

const removed = gcThrowawayProfiles()
if (removed.length) console.log(`[testapp] removed ${removed.length} leaked throwaway profile(s)`)

const inst = await launch(opts)
console.log(`[testapp] profile ${inst.profileDir}${inst.throwaway ? ' (throwaway)' : ''}`)
console.log(`[testapp] pid ${inst.pid}, CDP port ${inst.port}`)
console.log(`[testapp] app log ${inst.logFile}`)
console.log(`[testapp] e.g. node apps/testapp/cdp.mjs --port ${inst.port} call info`)

const {code, signal} = await inst.exited
if (inst.throwaway) deleteProfile(inst.profileDir)
process.exit(signal ? 1 : (code ?? 0))
