/**
 * Scenes from TS (source/coredcc/bind/ts/scene.ts): transform types whose model is a script's,
 * evaluating, picking and the `objects` input. run.mjs builds and runs it.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, describe, test} from 'node:test'
import {pathToFileURL} from 'node:url'

import * as cdcc from 'coredcc-script'
import {Ref, ScriptError, Session, StatusCode, type Context, type TransformTypeSpec} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as app from 'coredcc-gen/testapp/functions'

const create = (await import(pathToFileURL(process.env.SPIKE_MODULE as string).href)).default
cdcc.load(await create(), 'TESTAPP_GetManager')

const fails = (code: StatusCode, re: RegExp) => (e: unknown) =>
  e instanceof ScriptError && e.code === code && re.test(e.message)

const near = (a: ArrayLike<number>, b: ArrayLike<number>) => {
  assert.equal(a.length, b.length)
  for (let i = 0; i < a.length; i++) assert.ok(Math.abs(a[i] - b[i]) < 1e-9, `${Array.from(a)} isn't ${Array.from(b)}`)
}
const at = (x: number, y: number, z: number) => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1]
/** An orthographic view of x and y from -10 to 10, 100 pixels wide, with the nearest z at 10. */
const view: cdcc.ViewSpec = {
  viewport: [0, 0, 100, 100],
  view: at(0, 0, 0),
  proj: [0.1, 0, 0, 0, 0, 0.1, 0, 0, 0, 0, -0.05, 0, 0, 0, 0.5, 1],
  ortho: true,
}

const mul = (a: ArrayLike<number>, b: ArrayLike<number>) =>
  Array.from({length: 16}, (_, i) => {
    let sum = 0
    for (let k = 0; k < 4; k++) sum += a[k * 4 + (i % 4)] * b[Math.floor(i / 4) * 4 + k]
    return sum
  })

/** `radius` and `angle` in the plane of x and y, and a `height` that `tilt` turns off. */
const polar: TransformTypeSpec = {
  id: 'ts.polar',
  uiName: 'Polar',
  channels: {radius: cdcc.float(1), angle: cdcc.float(0), height: cdcc.float(0), flat: false},
  distances: ['radius', 'height'],
  evaluate({channels: [r, a, h, flat]}, parent) {
    return {
      world: mul(parent.world, at(r * Math.cos(a), r * Math.sin(a), flat ? 0 : h)),
      rotation: parent.rotation,
    }
  },
  setWorld({channels}, target, parent) {
    /* The parents of these tests are only moved. */
    const [x, y, z] = [12, 13, 14].map((i) => target[i] - parent.world[i])
    return [Math.hypot(x, y), Math.atan2(y, x), z, channels[3]]
  },
}

const suite = () => {
  let session: Session
  let ctx: Context
  const open = () => {
    const host = app.create() as Host
    session = new Session(host, app.context(host) as Context)
    ctx = session.ctx
    cdcc.setScene(ctx, cdcc.newScene(ctx, 'Scene'))
  }
  const validate = () => {
    const st = app.validate((ctx as unknown as {__host: Host}).__host)
    try {
      assert.ok(st.ok(), st.message())
    } finally {
      st[Symbol.dispose]()
    }
  }
  beforeEach(open)
  afterEach(() => {
    if (session.disposed) return
    validate()
    session[Symbol.dispose]()
  })

  const add = (args: string) => cdcc.invoke(ctx, `core.object.add(${args})`).object as Ref
  const where = (o: Ref) => cdcc.worldMatrix(ctx, o).slice(12, 15)

  test('scenes: made, set, evaluated and read by path', () => {
    const second = cdcc.newScene(ctx, 'Second')
    assert.ok(second instanceof Ref)
    assert.equal(cdcc.get(ctx, 'scene.name'), 'Scene')
    cdcc.setScene(ctx, second)
    assert.equal(cdcc.get(ctx, 'scene.name'), 'Second')
    cdcc.undo(ctx)
    assert.throws(() => cdcc.evaluateScene(ctx), fails(StatusCode.NotFound, /scene/))
    cdcc.setScene(ctx, null)
    assert.equal(cdcc.get(ctx, 'scene'), null)
    assert.throws(() => add('name="x"'), fails(StatusCode.FailedPrecondition, /no scene/))
    assert.throws(() => cdcc.setScene(ctx, second), fails(StatusCode.NotFound, /no block/))
    assert.throws(() => cdcc.setScene(ctx, new Ref('nope')), fails(StatusCode.InvalidArgument, /setScene/))
    cdcc.redo(ctx)
    cdcc.setScene(ctx, second)

    const a = add('name="a" location=(1, 2, 3)')
    near(cdcc.worldMatrix(ctx, a), at(1, 2, 3))
    cdcc.evaluateScene(ctx)
    const key = cdcc.blockIds(ctx, [a])
    near(cdcc.get(ctx, `evaluated.objects["${key}"].world`) as number[], at(1, 2, 3))
    assert.throws(() => cdcc.worldMatrix(ctx, second), fails(StatusCode.FailedPrecondition, /./))
    cdcc.undo(ctx)
    assert.throws(() => cdcc.worldMatrix(ctx, a), fails(StatusCode.NotFound, /worldMatrix/))
  })

  test('a transform type in TS', () => {
    cdcc.defineTransformType(ctx, polar)
    const root = add('name="root" location=(0, 0, 5)')
    const o = add(`type="ts.polar" name="o" parent="${root.text}" location=(3, 4, 7)`)
    near(where(o), [3, 4, 7])
    cdcc.setScene(ctx, cdcc.get(ctx, 'scene') as Ref)
    cdcc.evaluateScene(ctx)
    const path = `scene.objects[1].transform`
    assert.equal(cdcc.get(ctx, `${path}.radius`), 5)
    assert.equal(cdcc.get(ctx, `${path}.height`), 2)
    cdcc.set(ctx, `${path}.radius`, 10)
    near(where(o), [6, 8, 7])
    cdcc.set(ctx, `${path}.flat`, true)
    near(where(o), [6, 8, 5])
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    near(where(o), [3, 4, 7])

    /* Moved through setWorld, with the parent moving it too. */
    const both = cdcc.blockIds(ctx, [root, o])
    cdcc.invoke(ctx, `core.object.applyDelta(vector=(1, 0, 0) objects="${cdcc.blockIds(ctx, [o])}")`)
    near(where(o), [4, 4, 7])
    cdcc.invoke(ctx, `core.object.applyDelta(vector=(0, 1, 0) objects="${both}")`)
    near(where(o), [4, 5, 7])
    near(where(root), [0, 1, 5])
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    near(where(o), [3, 4, 7])
    /* As a TRS object, and back. */
    const only = cdcc.blockIds(ctx, [o])
    assert.equal(cdcc.invoke(ctx, `core.object.setTransformType(objects="${only}")`).approximate, 0)
    near(where(o), [3, 4, 7])
    cdcc.invoke(ctx, `core.object.setTransformType(type="ts.polar" objects="${only}")`)
    near(where(o), [3, 4, 7])
  })

  test('what the model is given, and applyDelta', () => {
    const seen: unknown[] = []
    cdcc.defineTransformType(ctx, {
      id: 'ts.delta',
      channels: {v: [1, 2, 3], n: 4, b: true},
      distances: ['v'],
      evaluate({channels: [x, y, z], time}) {
        return {world: at(x, y, z + time), localScale: [1, 2, 3]}
      },
      setWorld: (o, target) => [target[12], target[13], target[14] - o.time, 0, 0],
      applyDelta(base, nearTo, delta, parent) {
        seen.push([base, nearTo, delta, parent])
        return base.channels.map((c, i) => (i < 3 ? c + delta.vector[i] : c))
      },
    })
    const o = add('type="ts.delta" location=(1, 2, 3)')
    const path = `scene.objects[0].transform`
    assert.deepEqual(cdcc.get(ctx, `${path}.v`), [1, 2, 3])
    assert.equal(cdcc.get(ctx, `${path}.n`), 4) /* it is there already: the model wasn't asked */
    assert.equal(cdcc.get(ctx, `${path}.b`), true)
    cdcc.set(ctx, 'scene.fps', 10)
    cdcc.set(ctx, 'scene.frame', 5)
    near(where(o), [1, 2, 3.5])
    const only = cdcc.blockIds(ctx, [o])
    cdcc.invoke(
      ctx,
      `core.object.applyDelta(kind='TRANSLATE' vector=(1, 2, 3) pivot=(4, 5, 6) axis=(0, 1, 0) angle=0.5 objects="${only}")`
    )
    near(where(o), [2, 4, 6.5])
    assert.deepEqual(seen, [
      [
        {channels: [1, 2, 3, 4, 1], time: 0.5},
        [1, 2, 3, 4, 1],
        {kind: 'translate', basis: [0, 0, 0, 1], pivot: [4, 5, 6], vector: [1, 2, 3], axis: [0, 1, 0], angle: 0.5},
        {world: at(0, 0, 0), rotation: [0, 0, 0, 1], localScale: [1, 1, 1]},
      ],
    ])
    /* The state that children get. */
    let parent: cdcc.TransformState | undefined
    cdcc.defineTransformType(ctx, {
      ...polar,
      id: 'ts.child',
      evaluate(o, p) {
        parent = p
        return polar.evaluate(o, p)
      },
    })
    add(`type="ts.child" parent="${o.text}"`)
    cdcc.evaluateScene(ctx)
    assert.deepEqual(parent!.localScale, [1, 2, 3])
    near(parent!.world, at(2, 4, 6.5))
  })

  test('models that throw, or return what they should not', () => {
    let body: () => unknown = () => ({})
    let solve: () => unknown = () => [0]
    cdcc.defineTransformType(ctx, {
      id: 'ts.odd',
      channels: {x: cdcc.float(0)},
      distances: ['x'],
      evaluate: () => body() as object,
      setWorld: () => solve() as number[],
    })
    const o = add('type="ts.odd"')
    const other = add('type="core.transform.trs" name="other" location=(7, 0, 0)')
    const run = (f: () => unknown) => {
      body = f
      cdcc.evaluateScene(ctx)
    }
    run(() => ({}))
    near(where(o), [0, 0, 0])
    const boom = () => {
      throw new Error('boom')
    }
    assert.throws(() => run(boom), fails(StatusCode.Internal, /boom/))
    near(where(other), [7, 0, 0])
    assert.throws(() => cdcc.worldMatrix(ctx, o), fails(StatusCode.Internal, /boom/))
    assert.throws(() => run(async () => ({})), fails(StatusCode.Internal, /Promise/))
    assert.throws(() => run(() => undefined), fails(StatusCode.Internal, /no state/))
    assert.throws(() => run(() => ({world: [1, 2, 3]})), fails(StatusCode.Internal, /world has 16 numbers, not 3/))
    assert.throws(() => run(() => ({rotation: [1, 2, 3, '4']})), fails(StatusCode.Internal, /rotation: 4 isn't/))
    assert.throws(() => run(() => ({localScale: [1]})), fails(StatusCode.Internal, /localScale/))
    /* A state that isn't finite is no exception. */
    run(() => ({world: at(NaN, 0, 0)}))
    /* Nothing of the one before is left. */
    run(() => ({world: at(1, 2, 3), localScale: [4, 4, 4]}))
    run(() => ({}))
    near(where(o), [0, 0, 0])

    const only = cdcc.blockIds(ctx, [o])
    const move = (f: () => unknown) => {
      solve = f
      cdcc.invoke(ctx, `core.object.applyDelta(vector=(1, 0, 0) objects="${only}")`)
    }
    assert.throws(() => move(boom), fails(StatusCode.Internal, /boom/))
    const coded = () => {
      throw new ScriptError(StatusCode.OutOfRange, 'too far')
    }
    assert.throws(() => move(coded), fails(StatusCode.OutOfRange, /too far/))
    assert.throws(() => move(() => undefined), fails(StatusCode.Internal, /no channels/))
    assert.throws(() => move(() => [1, 2]), fails(StatusCode.Internal, /setWorld has 1 numbers, not 2/))
    assert.throws(() => move(() => ['1']), fails(StatusCode.Internal, /isn't a number/))
    assert.throws(() => move(async () => [1]), fails(StatusCode.Internal, /Promise/))
    /* The document is out of reach, and it can't be closed. */
    const busy = [
      () => cdcc.get(ctx, 'count'),
      () => cdcc.set(ctx, 'count', 1),
      () => cdcc.evaluateScene(ctx),
      () => cdcc.worldMatrix(ctx, o),
      () => cdcc.newScene(ctx, 'x'),
      () => cdcc.setScene(ctx, null),
      () => cdcc.pick(ctx, view, 50, 50, 1),
      () => cdcc.defineTransformType(ctx, {...polar, id: 'ts.inside'}),
    ]
    for (const f of busy) assert.throws(() => run(f as () => unknown), fails(StatusCode.Internal, /Busy|not from|evaluat/))
    assert.throws(() => run(() => session[Symbol.dispose]()), fails(StatusCode.Internal, /from inside/))
    assert.equal(cdcc.get(ctx, 'count'), 0)
    run(() => ({}))
  })

  test('registration errors', () => {
    const bad = (change: object, code: StatusCode, re: RegExp) =>
      assert.throws(() => cdcc.defineTransformType(ctx, {...polar, ...change}), fails(code, re))
    bad({id: 'core.polar'}, StatusCode.InvalidArgument, /reserved/)
    bad({id: 'polar'}, StatusCode.InvalidArgument, /dotted/)
    bad({channels: {length: 1}, distances: []}, StatusCode.InvalidArgument, /reserved/)
    bad({channels: {s: 'text'}, distances: []}, StatusCode.InvalidArgument, /String/)
    bad({channels: {n: 1}, distances: ['n']}, StatusCode.InvalidArgument, /distance/)
    assert.throws(() => cdcc.defineTransformType(ctx, {...polar, distances: ['nope']}), /no channel is named 'nope'/)
    assert.throws(() => cdcc.defineTransformType(ctx, {...polar, setWorld: 1 as never}), /setWorld must be/)
    assert.throws(() => cdcc.defineTransformType(ctx, {...polar, applyDelta: null as never}), /applyDelta must be/)
    cdcc.defineTransformType(ctx, polar)
    bad({}, StatusCode.AlreadyExists, /ts\.polar/)
    /* Another document has its own types. */
    session[Symbol.dispose]()
    assert.throws(() => cdcc.defineTransformType(ctx, polar), /disposed/)
    open()
    cdcc.defineTransformType(ctx, polar)
  })

  test('the objects input', () => {
    const a = add('name="a"')
    const b = add('name="b"')
    const selected = () => [0, 1].slice(0, cdcc.get(ctx, 'scene.selectedObjects.length') as number)
      .map((i) => cdcc.get(ctx, `scene.selectedObjects[${i}].name`))
    cdcc.invoke(ctx, `core.scene.select(objects="${cdcc.blockIds(ctx, [b, a, b])}")`)
    assert.deepEqual(selected(), ['b', 'a'])
    cdcc.invoke(ctx, `core.scene.select(mode='REMOVE' objects="${cdcc.blockIds(ctx, [b])}")`)
    assert.deepEqual(selected(), ['a'])
    assert.equal(cdcc.invoke(ctx, `core.object.delete(objects="${cdcc.blockIds(ctx, [b])}")`).count, 1)
    assert.throws(() => cdcc.blockIds(ctx, [b]), fails(StatusCode.NotFound, /no block/))
    cdcc.undo(ctx)
    assert.equal(cdcc.blockIds(ctx, [a, b]).split(';').length, 2)
    assert.throws(
      () => cdcc.invoke(ctx, 'core.object.delete(objects="nonsense")'),
      fails(StatusCode.InvalidArgument, /objects/)
    )
  })

  test('picking', () => {
    /* Nothing in the test app has bounds, so nothing is found; what is wrong is said. */
    add('name="a"')
    assert.deepEqual(cdcc.pick(ctx, view, 50, 50, 100), [])
    assert.throws(() => cdcc.pick(ctx, {...view, viewport: [0, 0, 0, 0]}, 1, 1, 1), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, reversedZ: true}, 1, 1, 1), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, depth: '-1..1'}, 1, 1, -1), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, proj: [1]}, 1, 1, 1), /proj has 16 numbers, not 1/)
    cdcc.setScene(ctx, null)
    assert.throws(() => cdcc.pick(ctx, view, 1, 1, 1), fails(StatusCode.FailedPrecondition, /no scene/))
  })
}

describe('scenes from TS', suite)
