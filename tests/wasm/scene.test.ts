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
    const key = cdcc.blockIdText(ctx, [a])
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
    const both = cdcc.blockIdText(ctx, [root, o])
    cdcc.invoke(ctx, `core.object.applyDelta(vector=(1, 0, 0) objects="${cdcc.blockIdText(ctx, [o])}")`)
    near(where(o), [4, 4, 7])
    cdcc.invoke(ctx, `core.object.applyDelta(vector=(0, 1, 0) objects="${both}")`)
    near(where(o), [4, 5, 7])
    near(where(root), [0, 1, 5])
    cdcc.undo(ctx)
    cdcc.undo(ctx)
    near(where(o), [3, 4, 7])
    /* As a TRS object, and back. */
    const only = cdcc.blockIdText(ctx, [o])
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
    cdcc.set(ctx, 'scene.units.fps', 10)
    cdcc.set(ctx, 'scene.frame', 5)
    near(where(o), [1, 2, 3.5])
    const only = cdcc.blockIdText(ctx, [o])
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
    /* The model answers otherwise: the scene hears of it, or its incremental pass keeps the result. */
    const run = (f: () => unknown) => {
      body = f
      cdcc.notifyChanged(ctx, o)
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

    const only = cdcc.blockIdText(ctx, [o])
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
      () => cdcc.pick(ctx, view, 50, 50),
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
    cdcc.invoke(ctx, `core.scene.select(objects="${cdcc.blockIdText(ctx, [b, a, b])}")`)
    assert.deepEqual(selected(), ['b', 'a'])
    cdcc.invoke(ctx, `core.scene.select(mode='REMOVE' objects="${cdcc.blockIdText(ctx, [b])}")`)
    assert.deepEqual(selected(), ['a'])
    assert.equal(cdcc.invoke(ctx, `core.object.delete(objects="${cdcc.blockIdText(ctx, [b])}")`).count, 1)
    assert.throws(() => cdcc.blockIdText(ctx, [b]), fails(StatusCode.NotFound, /no block/))
    cdcc.undo(ctx)
    assert.equal(cdcc.blockIdText(ctx, [a, b]).split(';').length, 2)
    assert.throws(
      () => cdcc.invoke(ctx, 'core.object.delete(objects="nonsense")'),
      fails(StatusCode.InvalidArgument, /objects/)
    )
  })

  test('picking', () => {
    /* Nothing in the test app has bounds, so nothing is found; what is wrong is said. */
    add('name="a"')
    assert.deepEqual(cdcc.pick(ctx, view, 50, 50, {radius: 100}), [])
    assert.throws(() => cdcc.pick(ctx, {...view, viewport: [0, 0, 0, 0]}, 1, 1), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, reversedZ: true}, 1, 1), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, depthRange: '-1..1'}, 1, 1, {radius: -1}), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => cdcc.pick(ctx, {...view, proj: [1]}, 1, 1), /proj has 16 numbers, not 1/)
    cdcc.setScene(ctx, null)
    assert.throws(() => cdcc.pick(ctx, view, 1, 1), fails(StatusCode.FailedPrecondition, /no scene/))
  })

  test('rays and regions of a view', () => {
    const ray = cdcc.rayFromPixel(view, 75, 25)
    near(ray.origin, [5, 5, 10])
    near(ray.dir, [0, 0, -1])
    const planes = cdcc.regionFromRect(view, 75, 25, 50, 40)
    assert.equal(planes.length, 24)
    const inside = (x: number, y: number, z: number) =>
      [0, 1, 2, 3, 4, 5].every((i) => {
        const [a, b, c, w] = planes.slice(i * 4, i * 4 + 4)
        return a * x + b * y + c * z + w >= 0
      })
    assert.ok(inside(0.1, 2.1, 0) && inside(4.9, 4.9, 9.9) && inside(4.9, 4.9, -9.9))
    assert.ok(!inside(-0.1, 3, 0) && !inside(1, 5.1, 0) && !inside(1, 1.9, 0) && !inside(1, 3, 10.1) && !inside(1, 3, -10.1))
    assert.throws(() => cdcc.rayFromPixel({...view, viewport: [0, 0, 0, 0]}, 1, 1), fails(StatusCode.InvalidArgument, /./))
  })

  test('units', () => {
    assert.equal(cdcc.format(ctx, 'scene.frame'), '0')
    assert.equal(cdcc.format(ctx, 'scene.units.lengthUnit'), 'Meter')
    cdcc.set(ctx, 'scene.units.lengthUnit', 'centimeter')
    add('name="Cube" location=(1, 2, 3)')
    const path = 'scene.objects["Cube"].transform.location'
    assert.equal(cdcc.format(ctx, path), '100 cm, 200 cm, 300 cm')
    assert.deepEqual(cdcc.parse(ctx, path, '1, 2 m, 0'), [0.01, 2, 0])
    assert.equal(cdcc.parse(ctx, path + '[0]', '5'), 0.05)
    assert.equal(cdcc.format(ctx, path), '100 cm, 200 cm, 300 cm')
    assert.throws(() => cdcc.parse(ctx, path, '1 kg'), fails(StatusCode.InvalidArgument, /Length unit 'kg'/))
    assert.throws(() => cdcc.format(ctx, 'scene.nothing'), fails(StatusCode.NotFound, /./))
    cdcc.set(ctx, 'scene.units.fps', 29.97)
    assert.equal(cdcc.get(ctx, 'scene.units.fps'), 30000 / 1001)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'scene.units.fps'), 24)
  })

  test('results, and what changed', () => {
    const a = add('name="a" location=(1, 0, 0)')
    const b = add(`name="b" parent="${a.text}" location=(3, 0, 0)`)
    assert.equal(cdcc.sceneResults(ctx).placeCount, 0)
    cdcc.evaluateScene(ctx)
    const all = cdcc.sceneResults(ctx)
    assert.equal(all.placeCount, 2)
    assert.deepEqual(all.status, [0, 0])
    assert.deepEqual(all.members, [1, 1])
    const objects = cdcc.sceneObjects(ctx).map((o) => o!.text)
    assert.deepEqual([...objects].sort(), [a.text, b.text].sort())
    const i = objects.indexOf(b.text)
    near(all.matrices.slice(i * 16, i * 16 + 16), at(3, 0, 0))
    assert.deepEqual([all.parents[i], all.parents[1 - i]], [1 - i, -1])
    assert.equal(all.bounds.length, 12)
    assert.ok(all.bounds[0] > all.bounds[3]) /* nothing to bound */
    assert.deepEqual(cdcc.sceneChildren(ctx, a).map((o) => o.text), [b.text])
    assert.deepEqual(cdcc.sceneChildren(ctx, null).map((o) => o.text), [a.text])
    assert.deepEqual(cdcc.sceneChildren(ctx, b), [])

    /* Placed, undoably. */
    assert.deepEqual(cdcc.setWorld(ctx, [b], at(5, 6, 7)), {approximate: 0, skipped: 0})
    assert.throws(() => cdcc.setWorld(ctx, [a, b], at(0, 0, 0)), /32 numbers, not 16/)
    cdcc.evaluateScene(ctx)
    const next = cdcc.sceneResults(ctx, [i])
    assert.equal(next.version, all.version + 1)
    assert.deepEqual([next.changed, next.allChanged, next.placeCount], [[i], false, 2])
    near(next.matrices, at(5, 6, 7))
    assert.deepEqual(cdcc.sceneObjects(ctx, [i]).map((o) => o!.text), [b.text])
    assert.throws(() => cdcc.sceneResults(ctx, [2]), fails(StatusCode.OutOfRange, /no such place/))
    cdcc.setWorld(ctx, [b, a], [...at(1, 2, 3), ...at(0, 0, 1)], true)
    near(where(a), [0, 0, 1])
    near(where(b), [1, 2, 3])
    cdcc.undo(ctx)
    near(where(b), [5, 6, 7])
  })

  test('a transform type with more', () => {
    let many = 0
    let kept: cdcc.ChannelEntries | undefined
    cdcc.defineTransformType(ctx, {
      id: 'ts.more',
      version: 2,
      channels: {
        axis: {default: 'Y', items: [{key: 'X', value: 0}, {key: 'Y', value: 1, uiName: 'Up'}]},
        offset: {
          default: cdcc.float(2),
          uiName: 'Offset',
          description: 'How far',
          subtype: 'Distance',
          unit: 'Length',
          displayUnit: 'cm',
          hardMin: 0,
          softMax: 10,
          precision: 2,
        },
        turn: cdcc.quat(0, 0, 0, 1),
      },
      distances: ['offset'],
      evaluate({channels: [axis, d]}, parent) {
        return {world: mul(parent.world, at(axis === 0 ? d : 0, axis === 1 ? d : 0, 0))}
      },
      setWorld: ({channels}) => channels,
      evaluateMany({count, channels, parents}) {
        many++
        const out: number[] = []
        for (let i = 0; i < count; i++) {
          const [axis, d] = channels.slice(i * 6)
          const parent = parents.slice(i * 23, i * 23 + 16)
          out.push(...mul(parent, at(axis === 0 ? d : 0, axis === 1 ? d : 0, 0)), 0, 0, 0, 1, 1, 1, 1)
        }
        return out
      },
      pivot: (_, parent) => [parent.world[12], 7, 0],
      upgrade(_, channels) {
        kept = channels
      },
    })
    const o = add('name="o" location=(0, 2, 0)')
    const only = cdcc.blockIdText(ctx, [o])
    cdcc.invoke(ctx, `core.object.setTransformType(type="ts.more" objects="${only}")`)
    const path = 'scene.objects["o"].transform'
    cdcc.set(ctx, `${path}.offset`, 4)
    cdcc.set(ctx, `${path}.axis`, 'X')
    cdcc.evaluateScene(ctx)
    assert.equal(many, 1)
    near(cdcc.sceneResults(ctx).matrices, at(4, 0, 0))
    near(where(o), [4, 0, 0])
    assert.deepEqual(cdcc.get(ctx, `${path}.turn`), [0, 0, 0, 1])
    const meta = cdcc.meta(ctx, `${path}.offset`)
    assert.equal(meta.uiName, 'Offset')
    assert.equal(meta.subtype, 'Distance')
    assert.equal(meta.unit, 'Length')
    assert.equal(cdcc.meta(ctx, `${path}.axis`).items![1].uiName, 'Up')
    assert.equal(kept, undefined) /* nothing is old */

    const bad = (channels: TransformTypeSpec['channels'], re: RegExp | ((e: unknown) => boolean)) =>
      assert.throws(() => cdcc.defineTransformType(ctx, {...polar, id: 'ts.bad', distances: [], channels}), re)
    bad({x: {default: 1, items: [{key: 'A', value: 1}]}}, /default is a key/)
    bad({x: {default: 'B', items: [{key: 'A', value: 1}]}}, fails(StatusCode.InvalidArgument, /no item 'B'/))
    bad({x: {default: 1.5, subtype: 'Bogus'}}, fails(StatusCode.InvalidArgument, /subtype 'Bogus'/))
    bad({x: {default: 1.5, hardMin: 2, hardMax: 1}}, fails(StatusCode.InvalidArgument, /./))
    assert.throws(
      () => cdcc.defineTransformType(ctx, {...polar, id: 'ts.bad', pivot: 1 as never}),
      /pivot must be a function/
    )
  })

  test('a Bvh', () => {
    /* Unit boxes at whole numbers along x, and one that has no bounds. */
    const count = 1000
    const bounds = new Float64Array(count * 6)
    for (let i = 0; i < count; i++) bounds.set([i, 0, 0, i + 0.5, 1, 1], i * 6)
    bounds.set([1, 1, 1, 0, 0, 0], 7 * 6)
    const bvh = new cdcc.Bvh()
    try {
      assert.equal(bvh.size, 0)
      assert.equal(bvh.rayClosest([0, 0, 0], [1, 0, 0]), null)
      bvh.build(bounds)
      assert.equal(bvh.size, count)
      assert.equal(bvh.growth, 1)
      const hit = bvh.rayClosest([6.75, 0.5, 0.5], [2, 0, 0])!
      assert.equal(hit.primitive, 8)
      assert.ok(Math.abs(hit.t - 0.625) < 1e-6)
      assert.equal(bvh.rayClosest([6.75, 0.5, 0.5], [2, 0, 0], 0, 0.5), null)
      assert.equal(bvh.rayClosest([6.75, 0.5, 0.5], [2, 0, 0], 2)!.primitive, 11)
      assert.equal(bvh.rayClosest([0, 0, 0], [0, 0, 0]), null)
      assert.deepEqual(bvh.rayAll([6.75, 0.5, 0.5], [1, 0, 0], 0, 3), [8, 9])
      assert.deepEqual(bvh.sphereOverlap([10.75, 0.5, 0.5], 0.3), [10, 11])
      assert.deepEqual(bvh.sphereOverlap([NaN, 0, 0], 1), [])
      const nearest = bvh.nearest([20.25, 3, 0.5])!
      assert.equal(nearest.primitive, 20)
      assert.ok(Math.abs(nearest.distance - 2) < 1e-6)
      assert.equal(bvh.nearest([20.25, 3, 0.5], 1), null)
      assert.deepEqual(bvh.region([1, 0, 0, -997.75]), [998, 999])
      assert.deepEqual(bvh.region([1, 0, 0]), [])
      /* Three are moved. */
      bvh.refit([0, 5, 0, 1, 6, 1, 1, 5, 0, 2, 6, 1, 2, 5, 0, 3, 6, 1], new Int32Array([900, 8, 9]))
      assert.deepEqual(bvh.rayAll([-1, 5.5, 0.5], [1, 0, 0]), [8, 9, 900])
      assert.equal(bvh.rayClosest([6.75, 0.5, 0.5], [1, 0, 0])!.primitive, 10)
      assert.ok(bvh.growth > 1)
      bvh.refit(bounds)
      assert.equal(bvh.rayClosest([6.75, 0.5, 0.5], [1, 0, 0])!.primitive, 8)
      assert.throws(() => bvh.build([1, 2, 3]), fails(StatusCode.InvalidArgument, /./))
      assert.throws(() => bvh.refit(bounds.subarray(6)), (e: unknown) => e instanceof ScriptError)
      assert.throws(() => bvh.refit([0, 0, 0, 1, 1, 1], [count]), fails(StatusCode.OutOfRange, /./))
      assert.throws(() => bvh.nearest([1, 2]), /point has 3 numbers, not 2/)
      assert.equal(bvh.size, count)
    } finally {
      bvh[Symbol.dispose]()
    }
  })
}

describe('scenes from TS', suite)
