/**
 * What the suites share: the module, and documents of it. run.mjs bundles each suite with its
 * own copy, and runs it in a process of its own.
 */
import assert from 'node:assert/strict'
import {pathToFileURL} from 'node:url'
import {setFlagsFromString} from 'node:v8'
import {runInNewContext} from 'node:vm'

import * as cdcc from 'coredcc-script'
import {ScriptError, Session, StatusCode, type Context} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as spike from 'coredcc-gen/testapp/functions'

export const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
export const module = await create()
export const backend = cdcc.load(module, 'TESTAPP_GetManager')

setFlagsFromString('--expose-gc')
export const gc = runInNewContext('gc') as () => void

/** A ScriptError with `code` whose message matches `re`. */
export const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)

export const open = () => {
  const host = spike.create() as Host
  return new Session(host, spike.context(host) as Context)
}

export const hostOf = (ctx: Context) => (ctx as unknown as {__host: Host}).__host

export const validate = (ctx: Context) => {
  const st = spike.validate(hostOf(ctx))
  try {
    assert.ok(st.ok(), st.message())
  } finally {
    st[Symbol.dispose]()
  }
}
