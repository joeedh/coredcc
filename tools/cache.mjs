#!/usr/bin/env node
// The build cache that every checkout of coredcc shares (Windows; needs the sccache fork
// with named-pipe support on PATH). See docs/debugging.md, "Build cache".
//
//   node tools/cache.mjs setup   write the cache's settings; run it again after adding or
//                                removing a worktree (new-worktree.mjs does)
//   node tools/cache.mjs stats   hits and misses
//   node tools/cache.mjs zero    reset the statistics
//   node tools/cache.mjs stop    stop the server
//
// Don't run sccache by hand here: a server started without these settings has no base
// directories, and one started without the pipe is a TCP server.

import {spawnSync} from 'node:child_process'
import crypto from 'node:crypto'
import fs from 'node:fs'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import {cacheFolder, checkouts, root, slashes} from './checkout.mjs'

const PIPE_VARIABLE = 'SCCACHE_SERVER_PIPE'

/** The first sccache on PATH, if it is one that can use a named pipe. */
function findSccache() {
  for (const dir of (process.env.PATH ?? '').split(path.delimiter)) {
    const exe = path.join(dir, 'sccache.exe')
    if (path.isAbsolute(dir) && fs.existsSync(exe)) {
      return fs.readFileSync(exe).includes(PIPE_VARIABLE) ? exe : null
    }
  }
  return null
}

/**
 * The settings of every client. A null unsets the variable. The disk settings are here and
 * not in sccache.toml because any of them in the environment replaces all of the file's.
 */
function settings(folder) {
  const hash = crypto.createHash('sha1').update(slashes(folder).toLowerCase()).digest()
  return {
    [PIPE_VARIABLE]: `sccache-coredcc-${hash.toString('hex').slice(0, 12)}`,
    // Not used, unless this sccache is replaced by one that doesn't know the pipe.
    SCCACHE_SERVER_PORT: String(20000 + (hash.readUInt16LE(0) % 20000)),
    SCCACHE_CONF: slashes(path.join(folder, 'sccache.toml')),
    SCCACHE_DIR: slashes(path.join(folder, 'objects')),
    SCCACHE_CACHE_SIZE: '5G',
    SCCACHE_LOCAL_RW_MODE: 'READ_WRITE',
    SCCACHE_IGNORE_SERVER_IO_ERROR: '1',
    SCCACHE_BASEDIRS: null,
    SCCACHE_DIRECT: null,
    SCCACHE_CLIENT_SIDE: null,
    SCCACHE_RECACHE: null,
    SCCACHE_MULTILEVEL_CHAIN: null,
  }
}

/** The launcher's arguments after `cmake -E env`, one per line. */
function launcherFile(folder, sccache) {
  const all = Object.entries(settings(folder))
  const lines = [
    ...all.filter(([, value]) => value === null).map(([name]) => `--unset=${name}`),
    ...all.filter(([, value]) => value !== null).map(([name, value]) => `${name}=${value}`),
    '--',
    slashes(sccache),
  ]
  return lines.join('\n') + '\n'
}

function run(folder, sccache, args, options = {}) {
  const env = {...process.env}
  for (const [name, value] of Object.entries(settings(folder))) {
    if (value === null) {
      delete env[name]
    } else {
      env[name] = value
    }
  }
  return spawnSync(sccache, args, {env, encoding: 'utf8', ...options})
}

/** Whether it succeeded, or had nothing to do because no server runs. */
function stopServer(folder, sccache) {
  const r = run(folder, sccache, ['--stop-server'])
  return r.status === 0 || r.status === 2
}

const readIfExists = (file) => (fs.existsSync(file) ? fs.readFileSync(file, 'utf8') : null)

/** Replaces `file` if its contents differ. Compiles that are starting hold it open. */
async function write(file, contents) {
  if (readIfExists(file) === contents) {
    return
  }
  const temporary = `${file}.${process.pid}`
  fs.writeFileSync(temporary, contents)
  const deadline = Date.now() + 20_000
  for (;;) {
    try {
      fs.renameSync(temporary, file)
      return
    } catch (e) {
      if (!['EPERM', 'EBUSY', 'EACCES'].includes(e.code) || Date.now() > deadline) {
        fs.rmSync(temporary, {force: true})
        throw e
      }
      await new Promise((resolve) => setTimeout(resolve, 5))
    }
  }
}

const wantedBasedirs = () => checkouts(root).map(({folder}) => slashes(folder))

/** As the server reports them. */
const normalized = (dirs) =>
  dirs
    .map((dir) => slashes(dir).toLowerCase().replace(/\/*$/, '/'))
    .sort()
    .join(';')

async function setup(folder, sccache) {
  fs.mkdirSync(path.join(folder, 'objects'), {recursive: true})
  await write(path.join(folder, 'launcher.txt'), launcherFile(folder, sccache))
  let dirs
  do {
    dirs = wantedBasedirs()
    // A JSON string array is a TOML one too.
    const toml = `# Written by coredcc's tools/cache.mjs.\nbasedirs = ${JSON.stringify(dirs)}\n`
    await write(path.join(folder, 'sccache.toml'), toml)
  } while (normalized(dirs) !== normalized(wantedBasedirs()))

  // Without a server this reports the file's, so there is nothing to stop then.
  const r = run(folder, sccache, ['--show-stats', '--stats-format', 'json'])
  let reported = null
  try {
    reported = JSON.parse(r.stdout).basedirs
  } catch {
    console.error(`cache: can't read the server's settings: ${r.stderr?.trim()}`)
  }
  if (reported && normalized(reported) !== normalized(dirs)) {
    if (!stopServer(folder, sccache)) {
      console.error("cache: the server has old settings and couldn't be stopped")
      process.exitCode = 1
      return
    }
    console.log('cache: stopped the server; the next compile starts one with the new settings')
  }
  console.log(`cache: ${folder} serves ${dirs.join(', ')}`)
  console.log('cache: build folders configured before the first setup need a new configure')
}

async function main(command) {
  const folder = process.platform === 'win32' ? cacheFolder() : null
  if (!folder) {
    console.log('cache: not available here (Windows, and a checkout of its own, only)')
    return
  }
  if (/[;\r\n]/.test(folder)) {
    console.error(`cache: the folder's name can't be used in CMake: ${folder}`)
    process.exitCode = 1
    return
  }
  const sccache = findSccache()
  if (!sccache) {
    console.log("cache: no sccache with named-pipe support on PATH; builds won't be cached")
    return
  }
  switch (command) {
    case 'setup':
      await setup(folder, sccache)
      break
    case 'stats':
    case 'zero': {
      const r = run(folder, sccache, [command === 'stats' ? '--show-stats' : '--zero-stats'], {
        stdio: 'inherit',
      })
      process.exitCode = r.status ?? 1
      break
    }
    case 'stop':
      process.exitCode = stopServer(folder, sccache) ? 0 : 1
      break
    default:
      console.error('usage: node tools/cache.mjs setup|stats|zero|stop')
      process.exitCode = 2
  }
}

if (path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  await main(process.argv[2])
}
