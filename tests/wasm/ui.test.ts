/**
 * What UIs are made of: layouts, watched paths, tools, and commands run with values.
 */
import assert from 'node:assert/strict'
import {afterEach, beforeEach, describe, test} from 'node:test'

import * as cdcc from 'coredcc-script'
import {CommandFlags, ScriptError, StatusCode, type Context, type ScriptCommand, type Session} from 'coredcc-script'
import * as spike from 'coredcc-gen/testapp/functions'
import type {Commands, Root} from 'coredcc-paths'

import {backend, fails, hostOf, open, validate} from './fixture.ts'

describe('coredcc wasm-st UI', () => {
  let session: Session
  let ctx: Context
  const handlesBefore = backend.foreignCount()

  beforeEach(() => {
    session = open()
    ctx = session.ctx
  })
  afterEach(() => {
    validate(ctx)
    session[Symbol.dispose]()
    /* The history released every instance, and the Runtime every factory. */
    assert.equal(backend.foreignCount(), handlesBefore)
  })

  test('layouts: nodes, issues, and a tool under a prefix', () => {
    const xml = `<layout path="items['Cube']">
  <prop path="weight" enabledIf="!/enabled"/>
  <tool invocation="ts.weigh(by=2)">Weigh</tool>
</layout>`
    const root = cdcc.parseLayout(xml)
    assert.equal(root.kind, 'layout')
    const [prop, tool] = root.children!
    assert.equal(prop.effectivePath, '/items["Cube"].weight')
    assert.equal(cdcc.get(ctx, prop.effectivePath), 1)
    assert.deepEqual(prop.enabledIf, {path: '/enabled', not: true})
    assert.equal(tool.label, 'Weigh')
    assert.equal(tool.prefix, '/items["Cube"]')
    assert.throws(() => cdcc.parseLayout(`<layout>
<x/></layout>`), fails(StatusCode.InvalidArgument, /layout 2:1/))

    const issues = cdcc.validateLayout(ctx, xml)
    assert.deepEqual(issues.map((i) => [i.severity, i.line, i.node, i.code]), [['Error', 3, 2, 'NotFound']])

    /* The command's own paths are under the prefix too, in its undo as well. */
    class Weigh implements ScriptCommand {
      before = 0
      exec(c: Context) {
        this.before = cdcc.get(c, 'weight') as number
        cdcc.write(c, 'weight', this.before + (cdcc.input(c, 'by') as number))
      }
      undo(c: Context) {
        cdcc.write(c, 'weight', this.before)
      }
    }
    cdcc.registerCommand(ctx, {id: 'ts.weigh', inputs: {by: 1}}, Weigh)
    assert.deepEqual(cdcc.validateLayout(ctx, xml), [])
    cdcc.invokeAt(ctx, tool.prefix!, tool.invocation!)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 3)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 1)
    /* Undo is where the run was, whatever the prefix names by then. */
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Ball"]') as cdcc.Ref)
    cdcc.invokeAt(ctx, 'activeItem', 'ts.weigh(by=2)')
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Cube"]') as cdcc.Ref)
    cdcc.write(ctx, 'items["Cube"].weight', 50)
    cdcc.undo(ctx)
    cdcc.redo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Ball"].weight'), 3)
    cdcc.write(ctx, 'items["Ball"].name', 'Sphere')
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, 'items["Sphere"].weight'), 1)
    assert.equal(cdcc.get(ctx, 'items["Cube"].weight'), 50)
    cdcc.write(ctx, 'items["Sphere"].name', 'Ball')
    assert.throws(() => cdcc.invokeAt(ctx, 'items["Cone"]', tool.invocation!), ScriptError)
    assert.throws(() => cdcc.invokeAt(ctx, '', tool.invocation!), fails(StatusCode.NotFound, /weight/))
  })

  test('a dynamic def: its keys, and watching what meta gives', () => {
    assert.equal(cdcc.meta(ctx, 'strength').dynamic, undefined)
    const focus = cdcc.meta(ctx, 'focus')
    assert.equal(focus.dynamic, true)
    assert.deepEqual(focus.items!.map((i) => i.key), ['NONE', 'Cube', 'Ball'])
    cdcc.set(ctx, 'focus[Ball]', true)
    assert.equal(cdcc.get(ctx, 'focus', {names: true}), 'Ball')
    assert.throws(() => cdcc.get(ctx, 'focus[Cone]'), fails(StatusCode.NotFound, /no key 'Cone'/))
    /* It can't be known without an instance. */
    const issues = cdcc.validateLayout(ctx, '<layout><prop path="focus[Cone]" visibleIf="focus[Cube]"/></layout>')
    assert.deepEqual(issues.map((i) => i.severity), ['Warning', 'Warning'])

    const reader = new cdcc.PathReader(ctx, ['level', 'focus', 'strength', 'focus[Ball]'])
    reader.watch()
    cdcc.set(ctx, 'count', 5)
    cdcc.set(ctx, 'items["Ball"].name', 'Sphere')
    cdcc.flushChanges(ctx)
    assert.deepEqual(reader.takeChangedMeta(), [0, 1, 3])
    assert.deepEqual(reader.takeChangedMeta(), [])
    assert.deepEqual(reader.takeChanged(), [3])
    assert.equal(cdcc.meta(ctx, 'level').hardMax, 5)
    assert.equal(reader.value(1, {names: true}), 'Sphere')
    cdcc.set(ctx, 'count', 6)
    cdcc.flushChanges(ctx)
    assert.deepEqual(reader.takeChanges().map((c) => [c.index, c.changed, c.metaChanged]), [[0, false, true]])
    /* A number that no item has any more. */
    cdcc.appendLibrary(ctx, cdcc.saveLibrary(ctx).bytes)
    cdcc.write(ctx, 'items[3].name', 'Other')
    cdcc.write(ctx, 'focus', 'Other')
    const focused = cdcc.get(ctx, 'focus')
    assert.equal(cdcc.get(ctx, 'focus', {names: true}), 'Other')
    cdcc.undo(ctx)
    reader.read()
    assert.deepEqual(reader.takeAll({names: true})[1].value, focused)
    assert.deepEqual(cdcc.get(ctx, 'focus', {names: true}), focused)
    reader[Symbol.dispose]()
  })

  test('PathReader reads, watches, and goes with its session', () => {
    const reader = new cdcc.PathReader(ctx, ['strength', 'mode', 'flags', 'items["Cone"].weight'])
    assert.equal(reader.count, 4)
    assert.throws(() => reader.value(0), fails(StatusCode.FailedPrecondition, /wasn't read/))
    reader.read()
    assert.equal(reader.value(0), 0.5)
    assert.equal(reader.value(1), 0)
    assert.equal(reader.value(1, {names: true}), 'OBJECT')
    assert.deepEqual(reader.value(2, {names: true}), [])
    assert.throws(() => reader.value(3), ScriptError)
    assert.throws(() => reader.value(4), fails(StatusCode.OutOfRange, /path 4/))
    assert.throws(() => reader.add('a..b'), ScriptError)
    assert.throws(() => new cdcc.PathReader(ctx, ['count', 'a..b']), ScriptError)

    reader.watch()
    assert.equal(spike.watched(hostOf(ctx)), 4)
    assert.equal(cdcc.flushChanges(ctx), 0)
    cdcc.set(ctx, 'strength', 0.25)
    cdcc.set(ctx, 'flags', ['HIDE'])
    assert.equal(reader.value(0), 0.5)
    assert.equal(cdcc.flushChanges(ctx), 2)
    assert.deepEqual(reader.takeChanged(), [0, 2])
    assert.deepEqual(reader.takeChanged(), [])
    assert.deepEqual(reader.value(2, {names: true}), ['HIDE'])
    /* A change made in C++. */
    spike.nudge(hostOf(ctx))
    assert.equal(cdcc.flushChanges(ctx), 1)
    assert.equal(reader.value(0), 0.375)
    /* All at once. */
    cdcc.set(ctx, 'mode', 'EDIT')
    cdcc.set(ctx, 'strength', 0.25)
    cdcc.flushChanges(ctx)
    const change = (index: number, value: unknown) => ({index, changed: true, metaChanged: false, value, type: index === 0 ? 'Float' : 'Enum'})
    assert.deepEqual(reader.takeChanges({names: true}), [change(0, 0.25), change(1, 'EDIT')])
    assert.deepEqual(reader.takeChanges(), [])
    cdcc.set(ctx, 'mode', 'OBJECT')
    cdcc.flushChanges(ctx)
    assert.deepEqual(reader.takeChanges(), [change(1, 0)])
    const all = reader.takeAll({names: true})
    assert.deepEqual(all.map((c) => [c.index, c.changed, c.value]), [
      [0, false, 0.25], [1, false, 'OBJECT'], [2, false, ['HIDE']], [3, false, undefined],
    ])
    assert.ok(all[3].error instanceof ScriptError && all[3].error.code === StatusCode.NotFound)
    assert.match(all[3].error.message, /^NotFound: no such item/)
    assert.match(all[3].error.detail, /^no such item/)
    assert.deepEqual(all.map((c) => c.type), ['Float', 'Enum', 'Flags', 'None'])
    reader.unwatch()
    assert.equal(spike.watched(hostOf(ctx)), 0)

    /* In a slot: reading, but not watching; a slot's context isn't a session's. */
    let inSlot: unknown[] = []
    class Slot implements ScriptCommand {
      exec(c: Context) {
        const tried = (fn: () => unknown) => {
          try {
            return fn()
          } catch (e) {
            return e instanceof ScriptError ? e.code : (e as Error).message
          }
        }
        inSlot = [
          tried(() => reader.read()),
          tried(() => reader.watch()),
          tried(() => cdcc.flushChanges(c)),
          tried(() => new cdcc.PathReader(c, ['count'])),
        ]
      }
      undo() {}
    }
    cdcc.registerCommand(ctx, {id: 'ts.slot'}, Slot)
    cdcc.invoke(ctx, 'ts.slot()')
    assert.deepEqual(inSlot, [undefined, StatusCode.Busy, StatusCode.Busy, "expected a session's context"])

    /* Disposed of by hand, or with the session, before its host. */
    const second = open()
    const other = new cdcc.PathReader(second.ctx, ['count'])
    other.watch()
    other[Symbol.dispose]()
    other[Symbol.dispose]()
    assert.equal(spike.watched(hostOf(second.ctx)), 0)
    assert.throws(() => other.read(), /disposed/)
    const late = new cdcc.PathReader(second.ctx, ['count'])
    late.watch()
    second[Symbol.dispose]()
    assert.throws(() => late.value(0), /disposed/)
    assert.throws(() => new cdcc.PathReader(second.ctx, ['count']), /disposed/)
  })

  /* Adds `by` to `weight` under the prefix, if it stays at most 10. */
  class Weigh implements ScriptCommand {
    before = 0
    canRun(c: Context) {
      const after = (cdcc.get(c, 'weight') as number) + (cdcc.input(c, 'by') as number)
      if (after > 10) throw new Error(`${after} is too heavy`)
    }
    exec(c: Context) {
      this.before = cdcc.get(c, 'weight') as number
      cdcc.write(c, 'weight', this.before + (cdcc.input(c, 'by') as number))
      cdcc.output(c, 'weight', cdcc.get(c, 'weight') as number)
    }
    undo(c: Context) {
      cdcc.write(c, 'weight', this.before)
    }
  }
  const weigh = () =>
    cdcc.registerCommand(
      ctx,
      {id: 'ts.weigh', inputs: {by: cdcc.float(1), note: ''}, outputs: {weight: cdcc.float(0)}},
      Weigh
    )
  /* Drags `weight` under the prefix. */
  class Drag implements ScriptCommand {
    before = 0
    after = 0
    exec(c: Context) {
      this.before = cdcc.get(c, 'weight') as number
    }
    undo(c: Context) {
      this.after = cdcc.get(c, 'weight') as number
      cdcc.write(c, 'weight', this.before)
    }
    redo(c: Context) {
      cdcc.write(c, 'weight', this.after)
    }
  }
  const cube = 'items["Cube"].weight'
  const ball = 'items["Ball"].weight'

  test('a PathReader gives what get gives, for values of each kind', () => {
    const paths = [
      'count', 'strength', 'label', 'mode', 'mode[EDIT]', 'flags', 'offset', 'offset[1]', 'items.length',
      'items["Cube"]', 'activeItem', 'big', 'enabled',
    ]
    const reader = new cdcc.PathReader(ctx, paths)
    const same = () => {
      reader.read()
      for (const names of [false, true]) {
        const all = reader.takeAll({names})
        assert.deepEqual(all.map((c) => c.value), paths.map((p) => cdcc.get(ctx, p, {names})))
      }
    }
    same()
    cdcc.write(ctx, 'big', 2n ** 60n + 1n)
    cdcc.write(ctx, 'label', '"\\\u2028é')
    cdcc.write(ctx, 'flags', ['SELECT', 'HIDE'])
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Ball"]') as cdcc.Ref)
    cdcc.write(ctx, 'offset', [1.5, -0.1, 3])
    same()
    cdcc.write(ctx, 'offset', [Infinity, -Infinity, 0])
    same()
    cdcc.write(ctx, 'big', -(2n ** 62n))
    same()
    reader[Symbol.dispose]()
  })

  test('1,000 changed values in one call, in time', (t) => {
    const reader = new cdcc.PathReader(ctx, Array.from({length: 1000}, (_, i) => ['strength', 'mode', 'offset'][i % 3]))
    reader.watch()
    cdcc.write(ctx, 'strength', 0.25)
    cdcc.write(ctx, 'mode', 'EDIT')
    cdcc.write(ctx, 'offset', [1, 2, 3])
    assert.equal(cdcc.flushChanges(ctx), 1000)
    const start = performance.now()
    assert.equal(reader.takeChanges({names: true}).length, 1000)
    const took = performance.now() - start
    t.diagnostic(`took ${took.toFixed(1)} ms`)
    assert.ok(took < 500)
    reader[Symbol.dispose]()
  })

  test('conditions, and containers of what is gone', () => {
    const reader = new cdcc.PathReader(ctx, [
      'enabled', 'label', 'items.length', 'activeItem', 'mode[EDIT]', 'mode', 'strength', 'flags', 'items[9]', 'items[0]',
    ])
    const holds = () => {
      reader.read()
      return reader.takeAll({names: true}).map((c) => {
        const on = cdcc.conditionHolds({path: ''}, c)
        assert.equal(cdcc.conditionHolds({path: '', not: true}, c), c.error ? false : !on)
        return on ? 1 : 0
      })
    }
    const there = () => reader.takeAll().map((c) => (cdcc.containerIsThere(c) ? 1 : 0))
    cdcc.write(ctx, 'enabled', false)
    assert.deepEqual(holds(), [0, 0, 1, 0, 0, 0, 0, 0, 0, 1])
    assert.deepEqual([there()[3], there()[8], there()[9]], [0, 0, 1])
    cdcc.write(ctx, 'enabled', true)
    cdcc.write(ctx, 'label', '0')
    cdcc.write(ctx, 'mode', 'EDIT')
    cdcc.write(ctx, 'flags', ['HIDE'])
    cdcc.write(ctx, 'activeItem', cdcc.get(ctx, 'items["Ball"]') as cdcc.Ref)
    assert.deepEqual(holds(), [1, 1, 1, 1, 1, 0, 0, 0, 0, 1])
    assert.equal(there()[3], 1)
    const unsupported = {index: 0, changed: true, metaChanged: false, type: 'None', value: undefined} as const
    assert.ok(cdcc.containerIsThere({...unsupported, error: new ScriptError(StatusCode.Unsupported, '')}))
    const empty = open()
    const count = new cdcc.PathReader(empty.ctx, ['count'])
    count.read()
    assert.equal(cdcc.conditionHolds({path: ''}, count.takeAll()[0]), false) /* 0 */
    empty[Symbol.dispose]()
  })

  test('run takes JS values, and canRun says why not', () => {
    weigh()
    const at = {prefix: '/items["Ball"]'}
    assert.deepEqual(cdcc.run(ctx, 'ts.weigh', {by: 2.5}, at), {weight: 3.5})
    assert.equal(cdcc.get(ctx, cube), 1)
    /* The last used, unless told not to; undefined isn't given. */
    assert.deepEqual(cdcc.run(ctx, 'ts.weigh', {by: undefined}, at), {weight: 6})
    assert.deepEqual(cdcc.run(ctx, 'ts.weigh', {}, {...at, lastUsed: false}), {weight: 7})
    assert.equal(cdcc.canRun(ctx, 'ts.weigh', {by: 4}, at), '11 is too heavy')
    assert.equal(cdcc.canRun(ctx, 'ts.weigh', {by: 3}, at), undefined)
    cdcc.set(ctx, 'commandDefaults["ts.weigh"].by', 9)
    assert.equal(cdcc.canRun(ctx, 'ts.weigh', {}, at), '16 is too heavy')
    assert.equal(cdcc.canRun(ctx, 'ts.weigh', {}, {...at, lastUsed: false}), undefined)
    assert.match(cdcc.canRun(ctx, 'ts.weigh')!, /no member 'weight'/)
    assert.match(cdcc.canRun(ctx, 'ts.wiegh')!, /no command 'ts.wiegh'/)
    assert.match(cdcc.canRun(ctx, 'ts.weigh', {bye: 1}, at)!, /bye/)
    assert.equal(cdcc.get(ctx, ball), 7)
    assert.throws(() => cdcc.run(ctx, 'ts.weigh', {by: 4}, at), fails(StatusCode.FailedPrecondition, /too heavy/))
    assert.throws(() => cdcc.run(ctx, 'ts.weigh', {by: 'x'}, at), ScriptError)
    assert.throws(() => cdcc.run(ctx, 'ts.weigh', {by: {} as never}, at), TypeError)
    assert.throws(() => cdcc.run(ctx, 'ts.weigh', {note: 'a\0b'}, at), fails(StatusCode.InvalidArgument, /NUL/))
    /* Nothing to quote. */
    const note = `"it's" \\ )(`
    cdcc.run(ctx, 'ts.weigh', {by: 0, note}, at)
    assert.deepEqual(cdcc.lastCommand(ctx), {id: 'ts.weigh', inputs: {by: 0, note}})
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, ball), 7)

    /* From exec, a sub-command. */
    class Both implements ScriptCommand {
      exec(c: Context) {
        for (const item of ['Cube', 'Ball']) cdcc.run(c, 'ts.weigh', {by: 1}, {prefix: `/items["${item}"]`})
        assert.match(cdcc.canRun(c, 'ts.weigh')!, /script/)
      }
      undo() {}
    }
    cdcc.registerCommand(ctx, {id: 'ts.both'}, Both)
    const before = spike.historySize(hostOf(ctx))
    cdcc.run(ctx, 'ts.both')
    assert.deepEqual([cdcc.get(ctx, cube), cdcc.get(ctx, ball)], [2, 8])
    assert.equal(spike.historySize(hostOf(ctx)), before) /* over the step undone */
    cdcc.undo(ctx)
    assert.deepEqual([cdcc.get(ctx, cube), cdcc.get(ctx, ball)], [1, 7])
  })

  test('numbers in units, and the parts of a vector', () => {
    const length = {unit: 'Length', displayUnit: 'cm', precision: 1}
    assert.equal(cdcc.formatNumber(0.0126, length), '1.3 cm')
    assert.equal(cdcc.parseNumber('1.5', length), 0.015)
    assert.equal(cdcc.parseNumber('2 m', length), 2)
    assert.throws(() => cdcc.parseNumber('2 kg', length), fails(StatusCode.InvalidArgument, /kg/))
    const strength = cdcc.meta(ctx, 'strength')
    assert.equal(cdcc.formatNumber(0.25, strength), '0.25')
    assert.equal(cdcc.parseNumber(' 0.25 ', strength), 0.25)
    assert.deepEqual(cdcc.meta(ctx, 'offset').components, ['X', 'Y', 'Z'])
    const y = cdcc.meta(ctx, 'offset[1]')
    assert.deepEqual([y.uiName, y.default, y.component], ['Y', 0, 1])
    assert.deepEqual([cdcc.meta(ctx, 'mode[OBJECT]').default, cdcc.meta(ctx, 'mode[EDIT]').default], [true, false])
  })

  test('the typed calls are the plain ones', () => {
    const doc = cdcc.typed<Root, Commands>(ctx)
    assert.equal(doc.ctx, ctx)
    doc.set('items["Cube"].weight', 4)
    doc.set('mode', 'EDIT')
    assert.equal(doc.get('items["Cube"].weight'), 4)
    assert.equal(doc.get('mode', {names: true}), 'EDIT')
    assert.equal(doc.get('mode'), 1)
    const drag = cdcc.beginGesture(ctx)
    for (const x of [0.6, 0.7]) doc.set('strength', x, drag)
    assert.equal(spike.historySize(hostOf(ctx)), 3)
    weigh()
    const at = {prefix: '/items["Cube"]'}
    const plain = cdcc.typed<Root, {'ts.weigh': Commands['ts.add']}>(ctx)
    assert.match(plain.canRun('ts.weigh', {amount: 1}, at)!, /amount/)
    assert.deepEqual(plain.run('ts.weigh', {}, at), {weight: 5})
  })

  test('canInvoke, and of many tools at once', () => {
    weigh()
    assert.equal(cdcc.canInvoke(ctx, '/items["Cube"]', 'ts.weigh(by=9)'), undefined)
    assert.equal(cdcc.canInvoke(ctx, '/items["Cube"]', 'ts.weigh(by=10)'), '11 is too heavy')
    assert.match(cdcc.canInvoke(ctx, '', 'ts.weigh(')!, /./)
    assert.equal(cdcc.get(ctx, cube), 1)

    const tools = new cdcc.ToolChecks(ctx)
    assert.equal(tools.add('/items["Cube"]', 'ts.weigh(by=5)'), 0)
    assert.equal(tools.add('/items["Ball"]', 'ts.weigh(by=50)'), 1)
    assert.throws(() => tools.add('a..b', 'ts.weigh'), ScriptError)
    assert.throws(() => tools.add('', 'ts weigh'), ScriptError)
    assert.equal(tools.add('', 'ts.nope'), 2)
    const first = tools.changed()
    assert.deepEqual(first.slice(0, 2), [{index: 0}, {index: 1, reason: '51 is too heavy'}])
    assert.match(first[2].reason!, /no command/)
    assert.deepEqual(tools.changed(), [])
    cdcc.set(ctx, cube, 6)
    assert.deepEqual(tools.changed(), [{index: 0, reason: '11 is too heavy'}])
    cdcc.undo(ctx)
    assert.deepEqual(tools.changed(), [{index: 0}])
    tools[Symbol.dispose]()
    tools[Symbol.dispose]()
    assert.throws(() => tools.changed(), /disposed/)
    const other = open()
    const late = new cdcc.ToolChecks(other.ctx)
    other[Symbol.dispose]()
    assert.throws(() => late.add('', 'ts.weigh'), /disposed/)
    assert.throws(() => new cdcc.ToolChecks(other.ctx), /disposed/)
  })

  test('interactive commands', () => {
    cdcc.registerCommand(ctx, {id: 'ts.drag', inputs: {note: ''}, flags: CommandFlags.Interactive}, Drag)
    weigh()
    assert.equal(cdcc.interactive(ctx), undefined)
    assert.throws(() => cdcc.run(ctx, 'ts.drag'), ScriptError)
    assert.throws(() => cdcc.beginInteractive(ctx, 'ts.weigh'), ScriptError)
    assert.throws(() => cdcc.writeInteractive(ctx, 'weight', 1), ScriptError)
    assert.throws(() => cdcc.commitInteractive(ctx), ScriptError)
    assert.throws(() => cdcc.cancelInteractive(ctx), ScriptError)
    for (const end of [cdcc.cancelInteractive, cdcc.commitInteractive]) {
      cdcc.beginInteractive(ctx, 'ts.drag', {note: 'x'}, {prefix: '/items["Ball"]'})
      assert.equal(cdcc.interactive(ctx), 'ts.drag')
      assert.throws(() => cdcc.set(ctx, 'count', 1), fails(StatusCode.Busy, /./))
      assert.ok(cdcc.canInvoke(ctx, '', 'ts.drag'))
      for (const w of [2, 3]) cdcc.writeInteractive(ctx, 'weight', w)
      assert.equal(cdcc.get(ctx, ball), 3)
      end(ctx)
      assert.equal(cdcc.interactive(ctx), undefined)
      assert.equal(cdcc.get(ctx, ball), end === cdcc.commitInteractive ? 3 : 1)
    }
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, ball), 1)
    cdcc.redo(ctx)
    assert.equal(cdcc.get(ctx, ball), 3)
  })

  test('the last command, again with other inputs', () => {
    weigh()
    assert.equal(cdcc.lastCommand(ctx), undefined)
    assert.throws(() => cdcc.rerunLast(ctx, {}), fails(StatusCode.FailedPrecondition, /no command/))
    cdcc.run(ctx, 'ts.weigh', {by: 2}, {prefix: '/items["Ball"]'})
    assert.deepEqual(cdcc.lastCommand(ctx), {id: 'ts.weigh', inputs: {by: 2, note: ''}})
    cdcc.rerunLast(ctx, {by: 5})
    assert.equal(cdcc.get(ctx, ball), 6)
    assert.equal(spike.historySize(hostOf(ctx)), 1)
    assert.throws(() => cdcc.rerunLast(ctx, {by: 50}), ScriptError)
    assert.throws(() => cdcc.rerunLast(ctx, {bye: 50}), ScriptError)
    assert.equal(cdcc.get(ctx, ball), 6)
    assert.deepEqual(cdcc.lastCommand(ctx)!.inputs, {by: 5, note: ''})
    cdcc.undo(ctx)
    assert.equal(cdcc.get(ctx, ball), 1)
    assert.equal(cdcc.lastCommand(ctx), undefined)
  })
})
