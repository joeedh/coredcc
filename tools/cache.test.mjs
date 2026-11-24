// node --test tools/cache.test.mjs
//
// The build cache and worktree tools, in a small repository made for the purpose, whose
// cache folder and server are its own. Needs Windows, Visual Studio and the sccache fork.

import assert from 'node:assert/strict'
import {spawnSync} from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import {after, before, test} from 'node:test'
import {root, slashes} from './checkout.mjs'

const temporary = fs.realpathSync.native(
  fs.mkdtempSync(path.join(os.tmpdir(), 'coredcc-cache-test-'))
)
const main = path.join(temporary, 'coredcc')
const tree = path.join(temporary, 'coredcc-topic-a')
const cache = path.join(temporary, '.coredcc-cache')
const hostile = path.join(temporary, 'hostile')

// A shell in which a build would use another cache, or a TCP server.
const env = {
  ...process.env,
  SCCACHE_BASEDIRS: slashes(hostile),
  SCCACHE_DIR: slashes(hostile),
  SCCACHE_CACHE_SIZE: '1G',
  SCCACHE_LOCAL_RW_MODE: 'READ_ONLY',
  SCCACHE_SERVER_PORT: '48127',
  // git, for the local submodule and the commits
  GIT_CONFIG_COUNT: '3',
  GIT_CONFIG_KEY_0: 'protocol.file.allow',
  GIT_CONFIG_VALUE_0: 'always',
  GIT_CONFIG_KEY_1: 'user.name',
  GIT_CONFIG_VALUE_1: 'test',
  GIT_CONFIG_KEY_2: 'user.email',
  GIT_CONFIG_VALUE_2: 'test@example.com',
}
delete env.SCCACHE_SERVER_PIPE
delete env.COREDCC_CACHE_DIR

function run(cwd, file, ...args) {
  const r = spawnSync(file, args, {cwd, env, encoding: 'utf8'})
  return {status: r.status, out: `${r.stdout}${r.stderr}`, stdout: r.stdout}
}
function ok(cwd, file, ...args) {
  const r = run(cwd, file, ...args)
  assert.equal(r.status, 0, `${file} ${args.join(' ')}\n${r.out}`)
  return r.stdout
}
const git = (cwd, ...args) => ok(cwd, 'git', ...args)
const node = (cwd, ...args) => ok(cwd, process.execPath, ...args)
const dev = (cwd, ...args) =>
  run(cwd, 'cmd', '/c', path.join(root, 'tools', 'devshell.cmd'), ...args)
const configure = (cwd, build, ...args) =>
  dev(cwd, 'cmake', '-G', 'Ninja', '-S', '.', '-B', build, '-DCMAKE_BUILD_TYPE=Debug',
    '-DCMAKE_C_COMPILER=cl', '-DCMAKE_CXX_COMPILER=cl', ...args)

const listening = (port) =>
  spawnSync('netstat', ['-ano', '-p', 'TCP'], {encoding: 'utf8'})
    .stdout.split('\n')
    .filter((line) => line.includes(`:${port} `) && line.includes('LISTENING')).length

const launcher = () => fs.readFileSync(path.join(cache, 'launcher.txt'), 'utf8').split('\n')
const sccacheEnv = () => {
  const pairs = launcher().filter((line) => /^SCCACHE_\w+=/.test(line))
  return {...process.env, ...Object.fromEntries(pairs.map((line) => line.split('=')))}
}
function stats() {
  const exe = launcher().at(-2)
  const r = spawnSync(exe, ['--show-stats', '--stats-format', 'json'], {
    env: sccacheEnv(),
    encoding: 'utf8',
  })
  const s = JSON.parse(r.stdout)
  const sum = (counts) => Object.values(counts.counts).reduce((a, b) => a + b, 0)
  return {
    hits: sum(s.stats.cache_hits),
    misses: sum(s.stats.cache_misses),
    basedirs: s.basedirs,
    location: s.cache_location,
  }
}

function files(dir) {
  return fs.existsSync(dir)
    ? fs.readdirSync(dir, {recursive: true, withFileTypes: true}).filter((e) => e.isFile()).length
    : 0
}

const found = spawnSync('where', ['sccache'], {encoding: 'utf8'})
const skip =
  process.platform !== 'win32' || found.status !== 0 ? 'needs Windows and sccache' : false

before(() => {
  if (skip) return
  const litestl = path.join(temporary, 'litestl')
  fs.mkdirSync(litestl)
  git(litestl, 'init', '-q', '-b', 'master')
  fs.writeFileSync(path.join(litestl, 'l.h'), '#pragma once\ninline int l() { return 1; }\n')
  git(litestl, 'add', '.')
  git(litestl, 'commit', '-q', '-m', 'one')

  fs.mkdirSync(main)
  git(main, 'init', '-q', '-b', 'master')
  for (const dir of ['tools', 'cmake']) {
    fs.cpSync(path.join(root, dir), path.join(main, dir), {recursive: true})
  }
  fs.copyFileSync(path.join(root, '.gitattributes'), path.join(main, '.gitattributes'))
  fs.writeFileSync(path.join(main, '.gitignore'), 'build*/\n')
  fs.writeFileSync(
    path.join(main, 'CMakeLists.txt'),
    `cmake_minimum_required(VERSION 3.25)
project(mini C CXX)
list(APPEND CMAKE_MODULE_PATH "\${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CoredccCache)
include_directories(extern/litestl "\${CMAKE_CURRENT_SOURCE_DIR}/inc")
add_library(mini STATIC a.cc b.cc)
`
  )
  fs.mkdirSync(path.join(main, 'inc'))
  fs.writeFileSync(path.join(main, 'inc', 'h.h'), '#pragma once\n#include "l.h"\n')
  for (const name of ['a', 'b']) {
    fs.writeFileSync(
      path.join(main, `${name}.cc`),
      `#include "h.h"\nint ${name}() { return l(); }\nconst char *${name}f() { return __FILE__; }\n`
    )
  }
  git(main, 'submodule', '--quiet', 'add', slashes(litestl), 'extern/litestl')
  git(main, 'config', 'core.hooksPath', 'tools/hooks')
  git(main, 'add', '.')
  git(main, 'commit', '-q', '-m', 'one')
})

after(() => {
  if (!skip && fs.existsSync(path.join(cache, 'launcher.txt'))) {
    node(main, 'tools/cache.mjs', 'stop')
  }
  fs.rmSync(temporary, {recursive: true, force: true, maxRetries: 20, retryDelay: 200})
})

test('setup writes the settings once', {skip}, () => {
  node(main, 'tools/cache.mjs', 'setup')
  const toml = path.join(cache, 'sccache.toml')
  assert.ok(fs.readFileSync(toml, 'utf8').includes(JSON.stringify([slashes(main)])))
  const {mtimeMs} = fs.statSync(toml)
  node(main, 'tools/cache.mjs', 'setup')
  assert.equal(fs.statSync(toml).mtimeMs, mtimeMs)
  assert.deepEqual(fs.readdirSync(cache).sort(), ['launcher.txt', 'objects', 'sccache.toml'])
  node(main, 'tools/cache.mjs', 'stop') // no server: not an error
})

test('the main checkout builds through the cache, whatever the shell sets', {skip}, () => {
  const before = listening(4226)
  const r = configure(main, 'build')
  assert.equal(r.status, 0, r.out)
  assert.match(r.out, /using the build cache/)
  const b = dev(main, 'cmake', '--build', 'build')
  assert.equal(b.status, 0, b.out)
  const s = stats()
  assert.deepEqual([s.hits, s.misses], [0, 2])
  assert.ok(slashes(s.location).includes(slashes(path.join(cache, 'objects'))), s.location)
  assert.ok(files(path.join(cache, 'objects')) >= 2)
  assert.equal(files(hostile), 0)
  assert.equal(listening(4226), before)
  assert.equal(listening(48127), 0)
})

test('a new worktree joins the running server and gets hits', {skip}, () => {
  node(main, 'tools/new-worktree.mjs', 'topic/a')
  assert.ok(fs.existsSync(path.join(tree, 'extern', 'litestl', 'l.h')))
  const common = git(path.join(tree, 'extern', 'litestl'), 'rev-parse', '--git-dir').trim()
  assert.ok(!fs.existsSync(path.join(common, 'objects', 'info', 'alternates')))

  const r = configure(tree, 'build')
  assert.equal(r.status, 0, r.out)
  const b = dev(tree, 'cmake', '--build', 'build')
  assert.equal(b.status, 0, b.out)
  const s = stats()
  assert.deepEqual([s.hits, s.misses], [2, 0]) // the server is a new one
  assert.equal(s.basedirs.length, 2)
  const deps = dev(tree, 'ninja', '-C', 'build', '-t', 'deps').out.replaceAll('\\', '/').toLowerCase()
  assert.match(deps, /h\.h/)
  assert.ok(!deps.includes(`${slashes(main).toLowerCase()}/`), deps)

  // a header edit in the worktree
  fs.appendFileSync(path.join(tree, 'inc', 'h.h'), 'inline int more() { return 2; }\n')
  assert.match(dev(tree, 'cmake', '--build', 'build').out, /\[\d\/3\]/)
  assert.equal(stats().misses, 2)
})

test('new-worktree resumes, and refuses what it must', {skip}, () => {
  fs.rmSync(path.join(tree, 'extern', 'litestl', 'l.h'))
  node(tree, 'tools/new-worktree.mjs', 'topic/a')
  for (const args of [['topic-a'], ['a;b'], ['--', '-x'], []]) {
    const r = run(main, process.execPath, 'tools/new-worktree.mjs', ...args)
    assert.equal(r.status, 1, `${args}: ${r.out}`)
  }
  node(main, 'tools/new-worktree.mjs', 'b')
  node(main, 'tools/new-worktree.mjs', 'cache')
  const r = run(main, process.execPath, 'tools/new-worktree.mjs', 'd')
  assert.equal(r.status, 1)
  assert.match(r.out, /3 worktrees already/)
  assert.equal(git(main, 'worktree', 'list').trim().split('\n').length, 4)
  assert.ok(fs.existsSync(path.join(cache, 'launcher.txt'))) // not the worktree `cache`'s folder
})

test('the pre-commit hook accepts a valid gitlink in a worktree, and no other', {skip}, () => {
  const litestl = path.join(temporary, 'litestl')
  git(litestl, 'commit', '-q', '--allow-empty', '-m', 'two')
  const sub = path.join(tree, 'extern', 'litestl')
  git(sub, 'fetch', '-q')
  git(sub, 'checkout', '-q', 'origin/master')
  git(tree, 'commit', '-q', '-m', 'bump', '--', 'extern/litestl')

  git(sub, 'commit', '-q', '--allow-empty', '-m', 'not on master')
  const r = run(tree, 'git', 'commit', '-q', '-m', 'bump', '--', 'extern/litestl')
  assert.notEqual(r.status, 0)
  assert.match(r.out, /not on litestl origin\/master/)
})

test('the option and the user launcher', {skip}, () => {
  const launched = () =>
    ['build.ninja', 'CMakeFiles/rules.ninja']
      .map((name) => fs.readFileSync(path.join(main, 'build-o', name), 'utf8'))
      .join('')
  let r = configure(main, 'build-o', '-DCOREDCC_SCCACHE=OFF')
  assert.match(r.out, /not using the build cache: COREDCC_SCCACHE is OFF/)
  assert.ok(!/sccache/i.test(launched().replaceAll(slashes(temporary), '')))
  r = configure(main, 'build-o', '-DCOREDCC_SCCACHE=ON')
  assert.match(r.out, /using the build cache/)
  assert.match(launched(), /SCCACHE_SERVER_PIPE=/)
  r = configure(main, 'build-o', '-DCMAKE_CXX_COMPILER_LAUNCHER=sccache')
  assert.match(r.out, /TCP\s+server/)
  r = configure(main, 'build-o', '-DCMAKE_CXX_COMPILER_LAUNCHER=', '-DCMAKE_C_COMPILER_LAUNCHER=')
  assert.ok(!launched().includes('SCCACHE_SERVER_PIPE='))
})
