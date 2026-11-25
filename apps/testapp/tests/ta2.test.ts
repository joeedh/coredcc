/**
 * TA2 (docs/plans/2026-11-24-p2-layout-library-io.md, "Step B4"): the property panel built from a
 * layout file. What the tests do to a widget goes through DOM events, and what they check is
 * read from the DOM and, through datapaths, from the document.
 */
import assert from 'node:assert/strict'
import {after, afterEach, before, beforeEach, describe, test} from 'node:test'
import {type App, ok, startApp} from './harness.ts'

interface NodeState {
  id: string
  kind: string
  label?: string
  visible: boolean
  enabled: boolean
  path?: string
  valueKind?: string
  value?: any
  error?: string
  reason?: string
  children?: NodeState[]
}

describe('TA2: the panel of a layout', () => {
  let app: App | undefined
  let missing: string | undefined
  let failed = false

  const call = async (method: string, ...args: unknown[]): Promise<any> => await app!.client.call(method, args)
  const value = async (method: string, ...args: unknown[]) => ok(await call(method, ...args))
  /** The error's message of a call that fails. */
  const refused = async (method: string, ...args: unknown[]): Promise<{name: string; message: string}> => {
    const r = await call(method, ...args)
    assert.equal(r.ok, false, `${method} should fail`)
    return r.error
  }
  const tracked = (fn: () => Promise<void>) => async () => {
    try {
      await fn()
    } catch (err) {
      failed = true
      throw err
    }
  }
  const step = (name: string, fn: () => Promise<void>) =>
    test(name, async (t) => {
      if (missing !== undefined) {
        t.skip(`the build has no coredcc backend: ${missing}`)
        return
      }
      await tracked(fn)()
    })

  /** The panel's nodes, by id. */
  const nodes = async (opts = {}): Promise<Map<string, NodeState>> => {
    const out = new Map<string, NodeState>()
    const add = (n: NodeState) => {
      out.set(n.id, n)
      n.children?.forEach(add)
    }
    add(await value('panelState', opts))
    return out
  }
  const node = async (id: string): Promise<NodeState> => {
    const n = (await nodes()).get(id)
    assert.ok(n, `no node '${id}'`)
    return n
  }
  const shows = async (id: string, want: unknown) => assert.deepEqual((await node(id)).value, want, id)

  before(
    tracked(async () => {
      app = await startApp({name: 'ta2'})
      const info = ok(await app.client.call('info'))
      if (info.backendMissing && !process.env.COREDCC_TESTAPP_REQUIRE_BACKEND) {
        missing = info.backendError
        return
      }
      assert.equal(info.backend, process.env.COREDCC_TESTAPP_BACKEND ?? 'wasm-st', info.backendError)
      assert.equal(info.build, 'ta2')
      await app.client.evaluate(`globalThis.errorsLogged = 0
        { const was = console.error; console.error = (...a) => { errorsLogged++; was(...a) } }`)
    })
  )
  after(async () => {
    await app?.close({failed})
  })
  beforeEach(
    tracked(async () => {
      if (missing !== undefined) return
      assert.deepEqual(await value('reset'), {foreignCountBetween: 0})
      assert.deepEqual((await value('panel', {file: 'testapp.xml'})).issues, [], 'the shipped layout has no issues')
    })
  )
  afterEach(
    tracked(async () => {
      if (missing !== undefined) return
      const c = await value('check')
      assert.ok(c.valid, c.message)
    })
  )

  step('shows every prop, with its name', async () => {
    const all = await nodes()
    const kinds = {
      label: 'String', count: 'Int', strength: 'Float', enabled: 'Bool', mode: 'Enum', editMode: 'EnumKey',
      flags: 'Flags', hide: 'EnumKey', offset: 'Vec3', offsetY: 'Component', big: 'Int', itemCount: 'Length',
      activeItem: 'Pointer',
    }
    for (const [id, kind] of Object.entries(kinds)) {
      assert.equal(all.get(id)?.valueKind, kind, id)
      assert.equal(all.get(id)?.error, undefined, id)
    }
    assert.equal(all.get('label')!.label, 'Title')
    assert.equal(all.get('count')!.label, 'Count')
    assert.equal(all.get('editMode')!.label, undefined)
    assert.equal(all.get('document')!.label, 'Document')
    assert.equal(all.get('strength')!.value, 0.5)
    assert.equal(all.get('mode')!.value, 'OBJECT')
    assert.equal(all.get('itemCount')!.value, '2')
    assert.deepEqual(all.get('offset')!.value, [0, 0, 0])
    assert.equal(all.get('activeItem')!.value, '')
  })

  step('every kind of widget writes', async () => {
    const edits: [string, unknown, string, unknown, (string | number)?][] = [
      ['label', 'hello', 'label', 'hello'],
      ['count', 7, 'count', 7],
      ['enabled', false, 'enabled', false],
      ['enabled', true, 'enabled', true],
      ['mode', 'EDIT', 'mode', 1],
      ['editMode', false, 'mode[EDIT]', false],
      ['hide', true, 'flags', 2],
      ['offset', 1.5, 'offset', [1.5, 0, 0], 0],
      ['offset', 2.5, 'offset', [1.5, 0, 2.5], 2],
      ['offsetY', -3, 'offset', [1.5, -3, 2.5]],
      ['big', '4611686018427387904', 'big', 2n ** 62n],
    ]
    let steps = 0
    for (const [id, typed, path, want, at] of edits) {
      const r = await call('panelSet', id, typed, at)
      if (id === 'editMode') {
        /* An enum's key can only be chosen, not taken away. */
        assert.ok(r.ok)
        continue
      }
      ok(r)
      assert.deepEqual(await value('get', path), want, id)
      steps++
    }
    assert.equal((await node('editMode')).error !== undefined, true)
    await shows('editMode', true)
    assert.equal((await value('history')).size, steps)
    await value('panelSet', 'flags', true, 'SELECT')
    assert.deepEqual(await value('get', 'flags', {names: true}), ['SELECT', 'HIDE'])
    await shows('flags', ['SELECT', 'HIDE'])
    await shows('hide', true)
    await shows('offset', [1.5, -3, 2.5])
    await shows('big', 2n ** 62n)
    /* And each undo shows. */
    await value('undo')
    await shows('flags', ['HIDE'])
    await value('undo')
    await shows('big', 0)
  })

  step('a refused write says why, and the old value comes back', async () => {
    await value('panelSet', 'activeItem', 'nonsense')
    const n = await node('activeItem')
    assert.equal(n.value, '')
    assert.match(n.error!, /reference/)
    const cube = await value('get', 'items["Cube"]')
    await value('panelSet', 'activeItem', cube.$ref)
    assert.equal((await node('activeItem')).error, undefined)
    /* Clamped: the widget shows what was written. */
    await value('panelSet', 'weight', 500)
    assert.equal(await value('get', 'items["Cube"].weight'), 100)
    await shows('weight', 100)
    /* What isn't a number is an error at the widget, and an emptied field has no number to read. */
    await value('panelSet', 'count', 4)
    const {size} = await value('history')
    await value('panelSet', 'count', '')
    assert.match((await node('count')).error!, /number|integer/)
    await shows('count', 4)
    assert.deepEqual([await value('get', 'count'), (await value('history')).size], [4, size])
    await value('panelType', 'count', '')
    await shows('count', '')
    await value('panelBlur', 'count')
    await shows('count', 4)
    assert.match((await refused('panelSet', 'count', 'x')).message, /can't hold/)
  })

  step('a pointer, there and back', async () => {
    assert.match((await node('itemName')).error!, /./)
    const cube = await value('get', 'items["Cube"]')
    await value('panelSet', 'activeItem', cube.$ref)
    assert.deepEqual(await value('get', 'activeItem'), cube)
    await shows('activeItem', cube.$ref)
    /* What is under it appears, with its widgets. */
    const n = await node('itemName')
    assert.equal(n.value, 'Cube')
    assert.equal(n.valueKind, 'String')
    assert.equal(n.error, undefined)
    await value('panelSet', 'other', (await value('get', 'items["Ball"]')).$ref)
    assert.equal(await value('get', 'activeItem.other.name'), 'Ball')
    await value('panelSet', 'activeItem', ' ')
    assert.equal(await value('get', 'activeItem'), null)
    assert.equal((await node('itemName')).valueKind, undefined)
    /* Through a path that leads elsewhere now. */
    await value('set', 'activeItem', await value('get', 'items["Ball"]'))
    await shows('itemName', 'Ball')
    await value('panelSet', 'itemName', 'Sphere')
    assert.equal(await value('get', 'items["Sphere"].name'), 'Sphere')
  })

  step('visibility and enabling follow their conditions', async () => {
    let all = await nodes()
    assert.equal(all.get('flags')!.visible, false)
    assert.equal(all.get('note')!.visible, false)
    assert.equal(all.get('mode')!.enabled, true)
    await value('panelSet', 'mode', 'EDIT')
    assert.equal((await node('flags')).visible, true)
    await value('panelSet', 'enabled', false)
    all = await nodes()
    assert.equal(all.get('note')!.visible, true)
    assert.equal(all.get('whenEnabled')!.enabled, false)
    for (const id of ['mode', 'flags', 'editMode']) assert.equal(all.get(id)!.enabled, false, id)
    assert.equal(all.get('hide')!.enabled, true)
    assert.match((await refused('panelSet', 'mode', 'OBJECT')).message, /disabled/)
    await value('undo')
    assert.equal((await node('mode')).enabled, true)
    assert.equal((await node('itemCount')).enabled, true)
    /* A widget that is made later, where it is disabled. */
    await value('panel', {
      xml: `<layout><column enabledIf="enabled"><prop id="w" path="activeItem.weight"/></column></layout>`,
    })
    await value('set', 'enabled', false)
    await nodes()
    await value('set', 'activeItem', await value('get', 'items["Cube"]'))
    const w = await node('w')
    assert.deepEqual([w.value, w.enabled], [1, false])
    const html = (await app!.client.evaluate(`document.getElementById('panel').innerHTML`)).value
    assert.match(html, /<input class="field" type="number" min="0" max="100"/)
  })

  step('a tool runs under its prefix, and is undone under it', async () => {
    await value('set', 'activeItem', await value('get', 'items["Ball"]'))
    assert.equal((await node('weigh')).reason, undefined)
    await value('panelClick', 'weigh')
    assert.equal(await value('get', 'items["Ball"].weight'), 3)
    await shows('weight', 3)
    await value('set', 'activeItem', await value('get', 'items["Cube"]'))
    assert.equal(await value('get', 'items["Cube"].weight'), 1)
    await value('undo')
    await value('undo')
    assert.equal(await value('get', 'items["Ball"].weight'), 1)
    await shows('weight', 1)
    await value('redo')
    assert.equal(await value('get', 'items["Ball"].weight'), 3)
    /* Its condition is from the root. */
    await value('set', 'enabled', false)
    assert.match((await (await nodes(), refused('panelClick', 'weigh'))).message, /disabled/)
    /* Without an item it can't run, and says so. */
    await value('set', 'enabled', true)
    await value('set', 'activeItem', null)
    await nodes()
    assert.match((await node('weigh')).reason!, /null/)
    assert.equal((await node('weigh')).error, undefined)
    assert.match((await refused('panelClick', 'weigh')).message, /disabled/)
    const title = `document.querySelector('#panel [data-id="weigh"] button').title`
    assert.match((await app!.client.evaluate(title)).value, /null/)
    await value('set', 'activeItem', await value('get', 'items["Ball"]'))
    assert.equal((await node('weigh')).reason, undefined)
    assert.equal((await app!.client.evaluate(title)).value, '')
  })

  step('a container of what is gone is hidden', async () => {
    assert.equal((await node('itemTab')).visible, false)
    await value('set', 'activeItem', await value('get', 'items["Ball"]'))
    assert.equal((await node('itemTab')).visible, true)
    await value('panel', {
      xml: `<layout>
        <column id="active" path="activeItem" visibleIf="enabled"><prop id="w" path="weight"/></column>
        <row id="nine" path="items[9]"/><panel id="zero" path="items[0]"/>
        <toolOptions target="testapp.addCount"/>
        <row id="wrong" path="activeItm"/><row id="odd" path="items[9]" enabledIf="nope"/><custom tag="x" id="custom" path="items[9]"/>
        <label id="some" visibleIf="label">x</label><label id="none" visibleIf="!label">x</label>
        <label id="has" visibleIf="/activeItem">x</label><label id="lots" visibleIf="items.length">x</label>
      </layout>`,
    })
    const visible = async () => {
      const all = await nodes()
      return ['active', 'nine', 'zero', 'wrong', 'odd', 'custom', 'some', 'none', 'has', 'lots'].filter((id) => all.get(id)!.visible)
    }
    await value('set', 'label', '')
    assert.deepEqual(await visible(), ['active', 'zero', 'wrong', 'custom', 'none', 'has', 'lots'])
    await value('set', 'label', 'x')
    await value('set', 'activeItem', null)
    assert.deepEqual(await visible(), ['zero', 'wrong', 'custom', 'some', 'lots'])
    await value('set', 'enabled', false)
    await value('set', 'activeItem', await value('get', 'items["Cube"]'))
    assert.deepEqual(await visible(), ['zero', 'wrong', 'custom', 'some', 'has', 'lots'])
  })

  step("a tool's options are its last-used inputs", async () => {
    await value('panelClick', 'addThree')
    assert.equal(await value('get', 'count'), 3)
    await shows('count', 3)
    await shows('addOptions.amount', 3)
    await value('panelSet', 'addOptions.amount', 10)
    assert.deepEqual(await value('exec', 'testapp.addCount'), {result: 13})
    await shows('count', 13)
    const button = await node('addThree')
    assert.equal(button.label, 'Add Count')
  })

  step('a drag is one undo step', async () => {
    const size = (await value('history')).size
    await value('panelDrag', 'strength', [0.25, 0.375, 0.75])
    assert.equal(await value('get', 'strength'), 0.75)
    await shows('strength', 0.75)
    assert.equal((await value('history')).size, size + 1)
    await value('panelDrag', 'strength', [0.5, 0.125])
    assert.equal((await value('history')).size, size + 2)
    await value('undo')
    await shows('strength', 0.75)
    await value('undo')
    await shows('strength', 0.5)
  })

  step('Escape takes back a drag', async () => {
    const was = await value('history')
    await value('panelDrag', 'strength', [0.25, 0.375], {cancel: true})
    assert.equal(await value('get', 'strength'), 0.5)
    await shows('strength', 0.5)
    assert.deepEqual(await value('history'), was)
    /* The next drag is one again. */
    await value('panelDrag', 'strength', [0.25, 0.75])
    assert.equal((await value('history')).size, was.size + 1)
    await shows('strength', 0.75)
  })

  step("dragging a number's name changes it", async () => {
    const {size} = await value('history')
    await value('panelScrub', 'count', [1, 3, 6])
    assert.equal(await value('get', 'count'), 6)
    await shows('count', 6)
    assert.equal((await value('history')).size, size + 1)
    await value('panelScrub', 'count', [2, 9], {cancel: true})
    assert.equal(await value('get', 'count'), 6)
    assert.equal((await value('history')).size, size + 1)
    await value('panelScrub', 'offsetY', [-2])
    assert.deepEqual(await value('get', 'offset'), [0, -0.125, 0])
    /* A click isn't a drag, and what isn't a number, or is disabled, isn't dragged. */
    await value('panelScrub', 'count', [])
    await value('panelScrub', 'label', [5])
    await value('panelScrub', 'itemCount', [5])
    assert.equal((await node('itemCount')).error, undefined)
    assert.equal((await value('history')).size, size + 2)
    await value('panel', {xml: `<layout><column enabledIf="enabled"><prop id="c" path="count"/></column></layout>`})
    await value('set', 'enabled', false)
    await nodes()
    await value('panelScrub', 'c', [5])
    assert.equal(await value('get', 'count'), 6)
    await value('set', 'enabled', true)
    await nodes()
    await value('panelScrub', 'c', [5])
    assert.equal(await value('get', 'count'), 11)
  })

  step('an edit that changes nothing shows what was written meanwhile', async () => {
    await value('panelType', 'count', '12')
    await value('set', 'count', 5)
    await shows('count', 12)
    await value('panelBlur', 'count')
    await shows('count', 5)
    assert.equal(await value('get', 'count'), 5)
  })

  step('a widget is made again when its def changes', async () => {
    const html = async () => (await app!.client.evaluate(`document.getElementById('panel').innerHTML`)).value
    const dom = async (expr: string) => (await app!.client.evaluate(expr)).value
    const keysShown = () =>
      dom(`[...document.querySelectorAll('#panel [data-id="focus"] .check')].map((c) => c.textContent).join()`)
    const slideMax = () => dom(`document.querySelector('#panel [data-id="slide"] input').max`)
    await value('panel', {
      xml: `<layout><prop id="level" path="level"/><prop id="focus" path="focus"/>
        <prop id="slide" path="level" widget="slider"/><prop id="name" path="items['Cube'].name"/></layout>`,
    })
    assert.match(await html(), /min="0" max="1"/)
    await value('set', 'count', 8)
    await nodes()
    assert.match(await html(), /min="0" max="8"/)
    /* Once: not with each value after it. */
    await dom(`document.querySelector('#panel [data-id="slide"] input').mine = true`)
    await value('panelSet', 'level', 5)
    await shows('slide', 5)
    assert.equal(await dom(`document.querySelector('#panel [data-id="slide"] input').mine`), true)
    assert.equal(await keysShown(), 'NONE,Cube,Ball')
    await value('panelSet', 'focus', true, 'Cube')
    await shows('focus', 'Cube')
    await value('panelSet', 'name', 'Brick')
    assert.equal(await keysShown(), 'NONE,Brick,Ball')
    await shows('focus', 'Brick')
    /* Not while it is dragged, but after. */
    await value('panelDrag', 'slide', [1, 2], {end: false})
    await value('set', 'count', 3)
    await nodes()
    assert.equal(await slideMax(), '8')
    await value('panelDrag', 'slide', [2])
    await nodes()
    assert.equal(await slideMax(), '3')
  })

  step('a change made in C++ reaches the panel', async () => {
    await value('nudge')
    await shows('strength', 0.625)
    /* By itself, without a call that flushes. */
    await value('nudge')
    for (let i = 0; ; i++) {
      const n = (await nodes({flush: false})).get('strength')!
      if (n.value === 0.75) break
      assert.ok(i < 100, `the panel still shows ${n.value}`)
      await new Promise((resolve) => setTimeout(resolve, 50))
    }
  })

  step('tabs, labels, separators and custom elements', async () => {
    await value('panelClick', 'toolTab')
    const html = (await app!.client.evaluate(`document.getElementById('panel').innerHTML`)).value
    assert.match(html, /class="node tab chosen" data-id="toolTab"/)
    assert.match(html, /class="node tab" data-id="itemTab"/)
    assert.match(html, /<hr>/)
    assert.match(html, /<input class="slider" type="range" min="0" max="1"/)
    assert.match(html, /data-attr-color="red" data-tag="swatch">Swatch</)
  })

  step('a hidden tab has no button, and another one is chosen', async () => {
    await value('panel', {
      xml: `<layout><tabs><tab id="one" label="One" visibleIf="enabled"><label id="a">x</label></tab>
        <tab id="two" label="Two"/><tab id="three" label="Three"/></tabs></layout>`,
    })
    const tabs = async () =>
      (await app!.client.evaluate(`JSON.stringify([...document.querySelectorAll('#panel .bar button')].map(
        (b) => (b.hidden ? '-' : b.textContent) + (b.classList.contains('chosen') ? '*' : ''))
        .concat([...document.querySelectorAll('#panel .tab.chosen')].map((t) => t.dataset.id)))`)).value
    assert.equal(await tabs(), '["One*","Two","Three","one"]')
    await value('set', 'enabled', false)
    await nodes()
    assert.equal(await tabs(), '["-","Two*","Three","two"]')
    await value('set', 'enabled', true)
    await nodes()
    assert.equal(await tabs(), '["One*","Two","Three","one"]')
    await value('panelClick', 'three')
    await value('set', 'enabled', false)
    await nodes()
    assert.equal(await tabs(), '["-","Two","Three*","three"]')
  })

  step('names, icons, tooltips, and how widgets are laid out', async () => {
    const {issues} = await value('panel', {
      xml: `<layout><panel id="shut" label="Shut" icon="box" closed="true" description="what"/>
        <panel id="open" label="Open"><row id="wide" expand="true"/></panel>
        <prop id="strength" path="strength"/>
        <prop id="bare" path="strength" icon="x" iconsOnly="true" description="mine"/>
        <prop id="flags" path="flags"/><prop id="focus" path="focus"/>
        <tool id="t" invocation="testapp.addCount" icon="plus" iconsOnly="true"/>
        <label id="l" icon="info">Hello</label>
        <prop id="prop" path="count"/><prop path="count"/><prop path="big"/></layout>`,
    })
    assert.deepEqual(issues, [])
    const dom = async (id: string, expr: string) =>
      (await app!.client.evaluate(
        `(() => { const el = document.querySelector('#panel [data-id="${id}"]'); return ${expr} })()`
      )).value
    const name = `[el.querySelector('.name').textContent, el.querySelector('.name .icon')?.dataset.icon, el.title].join()`
    assert.equal(await dom('shut', `el.querySelector('details').open`), false)
    assert.equal(await dom('open', `el.querySelector('details').open`), true)
    assert.equal(await dom('shut', `el.querySelector('summary .icon').dataset.icon + el.title`), 'boxwhat')
    assert.equal(await dom('wide', `el.classList.contains('expand')`), true)
    assert.equal(await dom('open', `el.classList.contains('expand')`), false)
    assert.equal(await dom('strength', name), 'Strength,bolt,How strong')
    assert.equal(await dom('bare', name), ',x,mine')
    assert.equal(await dom('bare', `el.querySelector('.name').title`), 'Strength')
    assert.equal(await dom('flags', `[...el.querySelectorAll('.check')].map((c) => c.textContent + '/' + c.title +
      '/' + c.querySelector('.icon')?.dataset.icon).join()`), '/Select/cursor,HIDE//undefined')
    assert.equal(await dom('focus', `el.querySelectorAll('input[type=radio]').length > 1`), true)
    const button = `[el.querySelector('button').textContent, el.querySelector('button').title].join()`
    assert.equal(await dom('t', button), ',Add Count')
    assert.equal(await dom('l', `el.textContent + el.querySelector('.icon').dataset.icon`), 'Helloinfo')
    /* A node without an id doesn't take an author's. */
    await value('panelSet', 'prop', 3)
    assert.deepEqual([(await node('prop')).value, (await node('prop-1')).value, (await node('prop-2')).value], [3, 3, 0])
  })

  step('a broken layout is refused, and the panel stays', async () => {
    const e = await refused('panel', {xml: '<layout><prp path="count"/></layout>'})
    assert.equal(e.name, 'ScriptError')
    assert.match(e.message, /layout 1:9/)
    assert.match((await refused('panel', {file: '../package.json'})).message, /layout file/)
    await value('panelSet', 'count', 5)
    await shows('count', 5)
    /* One that names what isn't there is shown, with its issues. */
    const {issues} = await value('panel', {
      xml: `<layout><prop id="a" path="cuont"/><tool id="b" invocation="testapp.nope()"/><prop id="c" path="count"/></layout>`,
    })
    assert.deepEqual(issues.map((i: any) => [i.severity, i.node]), [['Error', 1], ['Error', 2]])
    assert.match((await node('a')).error!, /did you mean.*count/)
    assert.match((await nodes({flush: false})).get('b')!.reason!, /nope/)
    assert.match((await refused('panelClick', 'b')).message, /disabled/)
    await shows('c', 5)
    assert.match((await refused('panelSet', 'a', 1)).message, /no input/)
  })

  step('reset closes the panel with the document', async () => {
    const watched = await value('watched')
    assert.ok(watched > 10)
    await value('panel', {file: 'testapp.xml'})
    assert.equal(await value('watched'), watched, "the panel that was replaced doesn't watch")
    await value('reset')
    assert.equal(await value('watched'), 0)
    assert.match((await refused('panelState')).message, /no panel/)
    assert.equal((await app!.client.evaluate(`document.getElementById('panel').innerHTML`)).value, '')
    /* Nor does its timer go on. */
    await new Promise((resolve) => setTimeout(resolve, 200))
    assert.equal((await app!.client.evaluate('globalThis.errorsLogged')).value, 0)
  })
})
