/**
 * Graphs in TS (source/coredcc/bind/ts/graph.ts): part of spike.test.ts's suite.
 */
import assert from 'node:assert/strict'
import {beforeEach, test} from 'node:test'

import * as cdcc from 'coredcc-script'
import {ScriptError, Session, StatusCode, type Context} from 'coredcc-script'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as spike from 'coredcc-gen/testapp/functions'

/** What the suite has: the document of the test that runs, and its helpers. */
export interface Env {
  readonly ctx: Context
  readonly session: Session
  open(): Session
  hostOf(ctx: Context): Host
  fails(code: StatusCode, re: RegExp): (e: unknown) => boolean
}

export function graphTests(env: Env) {
  const {open, hostOf, fails} = env
  let ctx: Context
  let session: Session
  beforeEach(() => {
    ctx = env.ctx
    session = env.session
  })

  /* graphs["g"] with a node of `type`, its `value` the output `out`, its `a` the input `in`. */
  const makeGraph = (type: string) => {
    const path = 'graphs["g"]'
    cdcc.newGraph(ctx, 'g')[Symbol.dispose]()
    const out = cdcc.addGraphInterface(ctx, path, 'output', 'out', 'float')
    const in_ = cdcc.addGraphInterface(ctx, path, 'input', 'in', 'float')
    const node = cdcc.addGraphNode(ctx, path, type, 10, 20)
    const output = cdcc.addGraphNode(ctx, path, 'graph.output')
    const input = cdcc.addGraphNode(ctx, path, 'graph.input')
    const link = cdcc.addGraphLink(ctx, path, node, 'value', output, `#${out}`)
    cdcc.addGraphLink(ctx, path, input, `#${in_}`, node, 'a')
    return {path, node, link, at: `${path}.nodes[#${node}]`}
  }

  test('a TS node type: evaluated, cached, edited, undone', () => {
    let runs = 0
    cdcc.defineNodeType(ctx, {
      id: 'ts.scale',
      uiName: 'Scale',
      inputs: {a: cdcc.float(1), offset: cdcc.float(0.5)},
      params: {factor: 2, label: 'x'},
      outputs: {value: 'float', text: 'string', copy: 'float'},
      evaluate(node) {
        runs++
        const a = node.input('a') as number
        node.setOutput('value', a * (node.param('factor') as number) + (node.input('offset') as number))
        node.setOutput('text', `${node.param('label')}${a}`)
        node.pass('a', 'copy')
      },
    })
    const {path, node, link, at} = makeGraph('ts.scale')
    assert.deepEqual(cdcc.get(ctx, `${at}.position`), [10, 20])
    using graph = new cdcc.GraphInstance(ctx, path)
    assert.equal(spike.graphInstances(hostOf(ctx)), 1)
    assert.equal(graph.evaluate('out'), 0.5)
    graph.setInput('in', 3)
    assert.equal(graph.evaluate('out'), 6.5)
    assert.equal(runs, 2)
    assert.equal(graph.evaluate('out'), 6.5)
    assert.equal(runs, 2)
    cdcc.set(ctx, `${at}.params.factor`, 10)
    assert.equal(graph.evaluate('out'), 30.5)
    cdcc.undo(ctx)
    assert.equal(graph.evaluate('out'), 6.5)
    graph.setInput('in', undefined)
    assert.equal(graph.evaluate('out'), 0.5)
    const steps = spike.historySize(hostOf(ctx))
    cdcc.removeGraphNode(ctx, path, node)
    assert.equal(graph.evaluate('out'), 0)
    cdcc.undo(ctx)
    assert.equal(graph.evaluate('out'), 0.5)
    assert.equal(spike.historySize(hostOf(ctx)), steps) /* instead of the one undone */
    cdcc.removeGraphLink(ctx, path, link) /* back with its id */
    assert.equal(graph.evaluate('out'), 0)
    assert.throws(() => graph.evaluate('no'), fails(StatusCode.NotFound, /no/))
    assert.throws(() => cdcc.addGraphNode(ctx, path, 'ts.sclae'), fails(StatusCode.NotFound, /ts\.scale/))
  })

  test('a TS kernel that throws, or misuses what it has', () => {
    let body: (node: cdcc.NodeContext) => unknown = () => {}
    let kept: cdcc.NodeContext | undefined
    cdcc.defineNodeType(ctx, {
      id: 'ts.odd',
      inputs: {a: cdcc.float(1)},
      outputs: {value: 'float'},
      evaluate(node) {
        kept = node
        return body(node) as void
      },
    })
    const {path, node} = makeGraph('ts.odd')
    using graph = new cdcc.GraphInstance(ctx, path)
    let value = 0
    const run = (f: typeof body) => {
      body = f
      graph.setInput('in', ++value) /* so that it runs */
      return graph.evaluate('out')
    }
    const boom = () => {
      throw new Error('boom')
    }
    assert.throws(() => run(boom), fails(StatusCode.Internal, /boom/))
    assert.match(graph.errorText('out'), new RegExp(`boom.*node ${node}`))
    const coded = () => {
      throw new ScriptError(StatusCode.OutOfRange, 'too far')
    }
    assert.throws(() => run(coded), fails(StatusCode.OutOfRange, /too far/))
    assert.throws(() => run(() => {}), fails(StatusCode.Internal, /value/)) /* no output */
    assert.throws(() => run(async () => {}), fails(StatusCode.Internal, /Promise/))
    assert.throws(() => run((n) => n.input('b')), fails(StatusCode.NotFound, /'b'/))
    assert.throws(() => run((n) => n.setOutput('value', 'x')), fails(StatusCode.TypeMismatch, /./))
    /* The document is out of reach, and it can't be closed. */
    assert.throws(() => run(() => cdcc.get(ctx, 'count')), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => cdcc.set(ctx, 'count', 1)), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => cdcc.newGraph(ctx, 'x')), fails(StatusCode.Busy, /kernel/))
    assert.throws(() => run(() => session[Symbol.dispose]()), fails(StatusCode.Internal, /from inside/))
    assert.equal(cdcc.get(ctx, 'count'), 0)
    /* And it works again. */
    assert.equal(run((n) => n.setOutput('value', (n.input('a') as number) + 0.5)), value + 0.5)
    assert.equal(graph.errorText('out'), '')
    assert.throws(() => kept!.input('a'), /already evaluated/)
  })

  test('a TS node type of all kinds of params, with a monitor, reloaded', () => {
    const spec = (factor: number, stop = false): cdcc.NodeTypeSpec => ({
      id: 'ts.kinds',
      inputs: {a: cdcc.float(1), m: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1]},
      params: {
        mode: cdcc.enumParam({ADD: 0, SUB: 4}, 'SUB'),
        bits: cdcc.flagsParam({X: 1, Y: 2}, ['Y']),
        wide: cdcc.flagsParam({A: 2 ** 31, B: 2 ** 40, C: 1n << 63n}, ['A', 'B', 'C']),
        turn: cdcc.quat(0, 0, 1, 0),
        item: cdcc.pointerParam('TestItem', {weak: true}),
        n: 1,
      },
      outputs: {value: 'float'},
      inputNames: {a: 'Amount'},
      outputNames: {value: 'Result'},
      ranges: {n: [0, 3], a: [0, 100]},
      outputAdapter: true,
      evaluate(node) {
        if (stop || node.cancelled) node.cancel()
        node.reportProgress(0.5, 'half')
        assert.equal(node.useBlock('item'), '')
        assert.deepEqual(node.param('turn'), [0, 0, 1, 0])
        assert.equal(node.param('bits'), 2)
        assert.equal(node.param('wide'), 2n ** 63n + 2n ** 40n + 2n ** 31n)
        const m = node.input('m') as number[]
        const linked = node.isLinked('a') ? 100 : 0
        node.setOutput('value', (node.param('mode') as number) * factor + m[13] + linked)
      },
    })
    cdcc.defineNodeType(ctx, spec(1))
    const {path, node, at} = makeGraph('ts.kinds')
    cdcc.set(ctx, `${at}.params.n`, 9)
    assert.equal(cdcc.get(ctx, `${at}.params.n`), 3)
    assert.equal(cdcc.get(ctx, `${at}.params.mode`, {names: true}), 'SUB')
    assert.deepEqual(cdcc.get(ctx, `${at}.params.bits`, {names: true}), ['Y'])
    using graph = new cdcc.GraphInstance(ctx, path)
    const out = graph.watch('out')
    const value = graph.watch('value', node)
    assert.deepEqual(graph.update(), [out.id, value.id])
    assert.deepEqual(graph.update(), [])
    assert.equal(out.value, 110)
    const at1 = out.changedAt
    assert.ok(at1 > 0 && out.isCurrent)
    cdcc.replaceNodeType(ctx, spec(2))
    assert.ok(!out.isCurrent)
    assert.deepEqual(graph.update(), [out.id, value.id])
    assert.equal(value.value, 114)
    assert.ok(out.changedAt > at1)
    const other = {...spec(2), outputs: {value: 'float', more: 'float'}}
    assert.throws(() => cdcc.replaceNodeType(ctx, other), fails(StatusCode.FailedPrecondition, /ts\.kinds/))
    assert.throws(
      () => cdcc.replaceNodeType(ctx, {...spec(2), id: 'ts.never'}),
      fails(StatusCode.NotFound, /ts\.never/)
    )

    /* Cancelled, and what there is to see of it. */
    cdcc.replaceNodeType(ctx, spec(2, true))
    assert.deepEqual(graph.update(), [out.id, value.id])
    assert.throws(() => out.value, fails(StatusCode.Cancelled, /cancelled/))
    assert.equal(out.lastGood, 114)
    assert.match(out.errorText, /cancelled/)
    const report = graph.report(node)
    assert.equal(report.state, 'Failed')
    assert.equal(report.error!.code, 'Cancelled')
    assert.equal(report.error!.node, String(node))
    cdcc.replaceNodeType(ctx, spec(3))
    assert.equal(graph.evaluate('out'), 118)
    assert.equal(graph.peek(node, 'value'), 118)
    assert.equal(graph.peek(`${node}`, 'a', 'input'), 0)
    assert.equal(graph.report(node).state, 'Ok')
    graph.drop(node)
    assert.equal(graph.report(node).state, 'NotEvaluated')
    assert.throws(() => graph.drop('1/2'), fails(StatusCode.NotFound, /./))
    graph.clear()
    graph.retry()
    graph.timeLimit = 1e-9
    assert.throws(() => graph.evaluate('out'), fails(StatusCode.Cancelled, /./))
    graph.timeLimit = 0
    assert.equal(graph.evaluate('out'), 118)
    value[Symbol.dispose]()
    assert.throws(() => value.value, fails(StatusCode.NotFound, /no watch/))
    assert.throws(() => cdcc.enumParam({A: 1}, 'B'), TypeError)
    assert.throws(() => graph.watch('value', '1/'), fails(StatusCode.InvalidArgument, /path/))

    /* Pasting, and links that don't work. */
    const id = cdcc.graphNewId(ctx, path)
    assert.ok(Number.isSafeInteger(id) && id !== node)
    assert.deepEqual(cdcc.graphDiagnostics(ctx, path), [])
  })

  test('tables: made, and computed by a TS kernel', () => {
    cdcc.defineNodeType(ctx, {
      id: 'ts.rows',
      inputs: {t: cdcc.payload('elements'), factor: cdcc.float(2)},
      outputs: {t: 'elements', value: 'float'},
      evaluate(node) {
        const t = node.inputTable('t')
        const factor = node.input('factor') as number
        const c = (t.columns.c ?? []).map((x) => cdcc.float((x as number) * factor))
        node.setOutput('t', new cdcc.Table(t.keys, {c, label: t.keys.map((k) => `k${k}`)} as never))
        node.setOutput('value', t.keys.length)
      },
    })
    const path = 'graphs["g"]'
    cdcc.newGraph(ctx, 'g')[Symbol.dispose]()
    const in_ = cdcc.addGraphInterface(ctx, path, 'input', 'in', 'elements')
    const out = cdcc.addGraphInterface(ctx, path, 'output', 'out', 'elements')
    const node = cdcc.addGraphNode(ctx, path, 'ts.rows')
    const input = cdcc.addGraphNode(ctx, path, 'graph.input')
    const output = cdcc.addGraphNode(ctx, path, 'graph.output')
    cdcc.addGraphLink(ctx, path, input, `#${in_}`, node, 't')
    cdcc.addGraphLink(ctx, path, node, 't', output, `#${out}`)
    using graph = new cdcc.GraphInstance(ctx, path)
    assert.deepEqual(graph.evaluateTable('out'), new cdcc.Table([], {c: [], label: []}))
    graph.setInput('in', new cdcc.Table([7, 3], {c: [1.5, 2]}))
    const want = new cdcc.Table([7, 3], {c: [3, 4], label: ['k7', 'k3']})
    assert.deepEqual(graph.evaluateTable('out'), want)
    assert.deepEqual(graph.peekTable(node, 't'), want)
    assert.deepEqual(graph.peekTable(node, 't', 'input'), new cdcc.Table([7, 3], {c: [1.5, 2]}))
    const watch = graph.watch('out')
    graph.update()
    assert.deepEqual(watch.table, want)
    assert.throws(() => graph.evaluate('out'), fails(StatusCode.Unsupported, /payload/))
    assert.throws(() => graph.setInput('in', new cdcc.Table([1, 1])), fails(StatusCode.InvalidArgument, /./))
    assert.throws(() => graph.setInput('in', new cdcc.Table([1], {c: []})), TypeError)
  })

  test('keys of 64 bits, through a foreach', () => {
    const path = 'graphs["g"]'
    const inner = 'graphs["inner"]'
    const each = cdcc.newGraph(ctx, 'inner')
    const c = cdcc.addGraphInterface(ctx, inner, 'input', 'c', 'float')
    const d = cdcc.addGraphInterface(ctx, inner, 'output', 'd', 'float')
    const innerIn = cdcc.addGraphNode(ctx, inner, 'graph.input')
    const innerOut = cdcc.addGraphNode(ctx, inner, 'graph.output')
    cdcc.addGraphLink(ctx, inner, innerIn, `#${c}`, innerOut, `#${d}`)
    cdcc.defineNodeType(ctx, {
      id: 'ts.keys',
      inputs: {t: cdcc.payload('elements')},
      outputs: {t: 'elements'},
      evaluate(node) {
        const t = node.inputTable('t')
        node.setOutput('t', new cdcc.Table(t.keys, t.columns))
      },
    })
    cdcc.newGraph(ctx, 'g')[Symbol.dispose]()
    const in_ = cdcc.addGraphInterface(ctx, path, 'input', 'in', 'elements')
    const out = cdcc.addGraphInterface(ctx, path, 'output', 'out', 'elements')
    const input = cdcc.addGraphNode(ctx, path, 'graph.input')
    const output = cdcc.addGraphNode(ctx, path, 'graph.output')
    const node = cdcc.addGraphNode(ctx, path, 'ts.keys')
    const foreach = cdcc.addGraphNode(ctx, path, 'graph.foreach')
    cdcc.set(ctx, `${path}.nodes[#${foreach}].group`, each)
    each[Symbol.dispose]()
    cdcc.addGraphLink(ctx, path, input, `#${in_}`, node, 't')
    cdcc.addGraphLink(ctx, path, node, 't', foreach, 'elements')
    cdcc.addGraphLink(ctx, path, foreach, 'elements', output, `#${out}`)
    using graph = new cdcc.GraphInstance(ctx, path)
    const keys = [2n ** 53n + 1n, 2 ** 53, 2n ** 63n - 1n, -(2n ** 63n), -(2n ** 53n) - 1n, 5]
    graph.setInput('in', new cdcc.Table(keys, {c: [1, 2, 3, 4, 5, 6]}))
    const got = graph.evaluateTable('out')
    assert.deepEqual(got.keys, keys)
    assert.deepEqual(got.columns, {d: [1, 2, 3, 4, 5, 6]})
    /* They name elements, too. */
    assert.equal(graph.peek(`${foreach}/${innerIn}[${2n ** 53n + 1n}]`, `#${c}`), 1)
    assert.equal(graph.peek(`${foreach}/${innerIn}[${2n ** 53n}]`, `#${c}`), 2)
    /* A bigint that is small is a number when it is read. */
    graph.setInput('in', new cdcc.Table([7n], {c: [1]}))
    assert.deepEqual(graph.evaluateTable('out').keys, [7])
    for (const bad of [0.5, NaN, Infinity, 2 ** 53 + 2, -(2 ** 60)]) {
      assert.throws(() => graph.setInput('in', new cdcc.Table([bad])), TypeError)
    }
    for (const bad of [2n ** 63n, -(2n ** 63n) - 1n]) {
      assert.throws(() => graph.setInput('in', new cdcc.Table([bad])), RangeError)
    }
  })

  test('a TS kernel that disposes of its own graph instance', () => {
    let graph: cdcc.GraphInstance | undefined
    cdcc.defineNodeType(ctx, {
      id: 'ts.dispose',
      inputs: {a: cdcc.float(1)},
      outputs: {value: 'float'},
      evaluate(node) {
        graph?.[Symbol.dispose]()
        node.setOutput('value', 7)
      },
    })
    const {path} = makeGraph('ts.dispose')
    graph = new cdcc.GraphInstance(ctx, path)
    assert.equal(graph.evaluate('out'), 7)
    assert.equal(spike.graphInstances(hostOf(ctx)), 0)
    graph[Symbol.dispose]()
    assert.throws(() => graph!.evaluate('out'), /graph instance was disposed/)
  })

  test('node type registration errors; an instance outlives its document', () => {
    const spec = {id: 'ts.n', outputs: {value: 'float'}, evaluate() {}}
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, id: 'graph.n'}), fails(StatusCode.InvalidArgument, /reserved/))
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, id: 'scene.transform.trs'}), fails(StatusCode.InvalidArgument, /reserved/))
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, outputs: {}}), fails(StatusCode.InvalidArgument, /output/))
    assert.throws(
      () => cdcc.defineNodeType(ctx, {...spec, inputs: {m: cdcc.payload('no.Mesh')}}),
      fails(StatusCode.NotFound, /no\.Mesh/)
    )
    assert.throws(() => cdcc.defineNodeType(ctx, {...spec, evaluate: 1 as never}), TypeError)
    cdcc.defineNodeType(ctx, spec)
    assert.throws(() => cdcc.defineNodeType(ctx, spec), fails(StatusCode.AlreadyExists, /ts\.n/))
    assert.throws(() => new cdcc.GraphInstance(ctx, 'count'), fails(StatusCode.InvalidArgument, /graph/))

    const other = open()
    cdcc.newGraph(other.ctx, 'g')[Symbol.dispose]()
    using late = new cdcc.GraphInstance(other.ctx, 'graphs["g"]')
    other[Symbol.dispose]()
    assert.throws(() => late.evaluate('out'), fails(StatusCode.FailedPrecondition, /closed/))
  })
}
