/**
 * Graphs that drive objects, from TS (enableDrivers and the scene's graphs in source/coredcc/bind/ts/scene.ts).
 * run.mjs builds and runs it.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, describe, test} from 'node:test'
import {pathToFileURL} from 'node:url'

import * as cdcc from 'coredcc-script'
import {Ref, ScriptError, Session, StatusCode, type Context} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as app from 'coredcc-gen/testapp/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
cdcc.load(await create(), 'TESTAPP_GetManager')

const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)

const suite = () => {
  let session: Session
  let ctx: Context
  beforeEach(() => {
    const host = app.create() as Host
    session = new Session(host, app.context(host) as Context)
    ctx = session.ctx
    cdcc.setScene(ctx, cdcc.newScene(ctx, 'Scene'))
  })
  afterEach(() => {
    if (!session.disposed) session[Symbol.dispose]()
  })

  const add = (args: string) => cdcc.invoke(ctx, `core.object.add(${args})`).object as Ref
  const where = (o: Ref) => cdcc.worldMatrix(ctx, o).slice(12, 15)
  const evaluatedAt = (o: Ref) => {
    cdcc.evaluateScene(ctx)
    return (cdcc.get(ctx, `evaluated.objects["${cdcc.blockIds(ctx, [o])}"].world`) as number[]).slice(12, 15)
  }

  /* graphs["g"]: `follower`'s location is `leader`'s. */
  const follow = (leader: Ref, follower: Ref) => {
    cdcc.graphCreate(ctx, 'g')[Symbol.dispose]()
    const path = 'graphs["g"]'
    const reader = cdcc.graphAddNode(ctx, path, 'scene.object.state')
    cdcc.set(ctx, `${path}.nodes[#${reader}].params.object`, leader)
    const driver = cdcc.graphAddNode(ctx, path, 'scene.drive.vec3')
    cdcc.set(ctx, `${path}.nodes[#${driver}].params.object`, follower)
    cdcc.set(ctx, `${path}.nodes[#${driver}].params.channel`, 'location')
    cdcc.graphLink(ctx, path, reader, 'location', driver, 'value')
    return {path, graph: cdcc.get(ctx, path) as Ref, reader, driver}
  }

  test('one object follows another: listed, evaluated, undone', () => {
    cdcc.enableDrivers(ctx)
    assert.throws(() => cdcc.enableDrivers(ctx), fails(StatusCode.AlreadyExists, /enabled/))
    const a = add('name="a" location=(3, 4, 5)')
    /* add remembers its inputs: without a location, b would be where a is. */
    const b = add('name="b" location=(0, 0, 0)')
    const {graph} = follow(a, b)
    assert.deepEqual(evaluatedAt(b), [0, 0, 0])
    cdcc.addDriverGraph(ctx, graph)
    assert.deepEqual(evaluatedAt(b), [3, 4, 5])
    assert.deepEqual(where(b), [3, 4, 5])
    cdcc.invoke(ctx, `core.object.applyDelta(vector=(1, 0, 0) objects="${cdcc.blockIds(ctx, [a])}")`)
    assert.deepEqual(evaluatedAt(b), [4, 4, 5])
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    assert.deepEqual(evaluatedAt(b), [0, 0, 0])
    cdcc.redo(ctx)
    assert.deepEqual(evaluatedAt(b), [3, 4, 5])
    assert.deepEqual(cdcc.driverDiagnostics(ctx), [])
    assert.equal(cdcc.get(ctx, 'driverGraphs.length'), 1)
    cdcc.removeDriverGraph(ctx, graph)
    assert.equal(cdcc.get(ctx, 'driverGraphs.length'), 0)
    assert.deepEqual(evaluatedAt(b), [0, 0, 0])
  })

  test('diagnostics, and drivers disabled', () => {
    assert.throws(() => cdcc.driverDiagnostics(ctx), fails(StatusCode.FailedPrecondition, /enabled/))
    cdcc.enableDrivers(ctx)
    const a = add('name="a"')
    const {path, graph, driver} = follow(a, a)
    cdcc.set(ctx, `${path}.nodes[#${driver}].params.channel`, 'nowhere')
    cdcc.addDriverGraph(ctx, graph)
    const found = cdcc.driverDiagnostics(ctx)
    assert.ok(found.length > 0, JSON.stringify(found))
    assert.ok(found.some((d) => /cycle|nowhere/.test(d.message)), JSON.stringify(found))
    assert.ok(found.every((d) => d.graph instanceof cdcc.Ref && d.graph.text === graph.text), JSON.stringify(found))
    cdcc.disableDrivers(ctx)
    cdcc.evaluateScene(ctx)
    cdcc.disableDrivers(ctx) /* nothing to do */
  })
}

describe('graphs that drive objects, from TS', suite)
