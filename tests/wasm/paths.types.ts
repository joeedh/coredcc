/**
 * What gen-paths makes of the test app's structs and commands, and the calls that go by it
 * (typed.ts), checked by tsc (run.mjs): a wrong type fails, and so does a `@ts-expect-error`
 * without an error. Nothing here runs.
 */
import {
  get,
  run,
  type At,
  type FloatValue,
  type InputsOf,
  type NamedOf,
  type NamedValue,
  type OutputsOf,
  type Ref,
  type RefTo,
  type Typed,
  type Value,
  type ValueOf,
  type Writable,
  type WritableOf,
} from 'coredcc-script'
import type {CommandId, Commands, Prop, Root, Structs, TestApp, TestItem} from 'coredcc-paths'
import type * as odd from 'coredcc-odd-paths'

type Equal<A, B> = (<T>() => T extends A ? 1 : 2) extends <T>() => T extends B ? 1 : 2 ? true : false
const check = <T extends true>(_: T) => {}
type Int = number | bigint

/* ---- what is generated ---- */

check<Equal<Root, TestApp>>(true)
check<Equal<Structs['TestItem'], TestItem>>(true)
check<Equal<TestItem['$struct'], 'TestItem'>>(true)
check<Equal<TestApp['count'], Prop<'Int'>>>(true)
check<Equal<TestApp['mode'], Prop<'Enum', 'OBJECT' | 'EDIT'>>>(true)
check<Equal<TestApp['focus'], Prop<'Enum', (string & {}) | 'NONE'>>>(true) /* dynamic */
check<Equal<TestApp['flags'], Prop<'Flags', 'SELECT' | 'HIDE'>>>(true)
check<Equal<TestApp['activeItem'], Prop<'Pointer', never, TestItem>>>(true)
check<Equal<TestApp['items'], Prop<'Collection', never, TestItem, 'ReadOnly' | 'Active'>>>(true)
check<Equal<keyof Commands['ts.add']['inputs'], 'amount' | 'default'>>(true)
const id: CommandId = 'ts.add'
void id
// @ts-expect-error: no such command
const none: CommandId = 'ts.nope'
void none
type Defaults = Structs['CommandDefaults:ts.add']
check<Equal<keyof Defaults, '$struct' | 'amount' | 'default'>>(true)

/* The odd catalog's (gen_paths.ts): `string` is a struct there, as `string_2`. */
check<Equal<odd.Root, odd.string_2>>(true)
check<Equal<odd.string_2, {readonly $struct: 'string'; x: odd.Prop<'Int'>}>>(true)
check<Equal<odd.a_b['x'], odd.Prop<'String'>>>(true) /* its own hides the inherited one */
check<Equal<odd.a_b_2['x'], odd.Prop<'String'>>>(true)
check<Equal<keyof odd.a_b_2, '$struct' | 'x' | '__proto__' | 'constructor' | 'odd "name"\n'>>(true)
check<Equal<odd._3d, {readonly $struct: '3d'; c: odd.Prop<'Int'>}>>(true)
check<Equal<odd.Anystring_2_2, odd.string_2 | odd.a_b | odd.a_b_2>>(true)
check<Equal<odd.Kinds['e'], odd.Prop<'Enum', 'a"|never\''>>>(true)
check<Equal<odd.Kinds['none'], odd.Prop<'Enum'>>>(true)
check<Equal<odd.Kinds['open'], odd.Prop<'Enum', string>>>(true)
check<Equal<odd.Kinds['openFlags'], odd.Prop<'Flags', (string & {}) | 'A'>>>(true)
check<Equal<odd.Kinds['p'], odd.Prop<'Pointer', never, odd.Anystring_2_2>>>(true)
check<Equal<odd.Kinds['q'], odd.Prop<'Pointer', never, odd.a_b_2 | odd.Kinds>>>(true)
check<Equal<odd.Kinds['gone'], odd.Prop<'Pointer', never, unknown>>>(true)
check<Equal<odd.Kinds['any'], odd.Prop<'Pointer', never, unknown>>>(true)
check<Equal<odd.Kinds['none2'], odd.Prop<'Pointer', never, unknown>>>(true)
check<Equal<odd.Kinds['list'], odd.Prop<'Collection', never, odd.Anycycle, 'Active'>>>(true)
check<Equal<odd.Kinds['plain'], odd.Prop<'Collection', never, odd.Kinds>>>(true)
check<Equal<odd.Kinds['m'], odd.Prop<'Mat4', never, never, 'ReadOnly'>>>(true)
check<Equal<odd.Kinds['maybe'], odd.Prop<'Int', never, never, 'ReadOnly' | 'Optional'>>>(true)
check<Equal<odd.Kinds['new'], odd.Prop<'Newer'>>>(true)
check<Equal<odd.CommandId, 'a"\'b'>>(true)

/* ---- the types of values ---- */

check<Equal<ValueOf<TestApp['count']>, Int>>(true)
check<Equal<ValueOf<TestApp['strength']>, number>>(true)
check<Equal<ValueOf<TestApp['enabled']>, boolean>>(true)
check<Equal<ValueOf<TestApp['label']>, string>>(true)
check<Equal<ValueOf<TestApp['offset']>, [number, number, number]>>(true)
check<Equal<ValueOf<TestApp['mode']>, number>>(true)
check<Equal<NamedOf<TestApp['mode']>, 'OBJECT' | 'EDIT' | number>>(true)
check<Equal<ValueOf<TestApp['flags']>, Int>>(true)
check<Equal<NamedOf<TestApp['flags']>, ('SELECT' | 'HIDE')[] | Int>>(true)
check<Equal<ValueOf<TestApp['activeItem']>, RefTo<'TestItem'> | null>>(true)
check<Equal<ValueOf<TestApp['items']>, never>>(true)
check<Equal<ValueOf<odd.Kinds['maybe']>, Int | undefined>>(true)
check<Equal<ValueOf<odd.Kinds['new']>, NamedValue>>(true)
check<Equal<ValueOf<odd.Kinds['m']>, number[]>>(true)
check<Equal<ValueOf<odd.Kinds['p']>, RefTo<'string' | 'a b' | 'a-b'> | null>>(true)
check<Equal<ValueOf<odd.Kinds['any']>, RefTo | null>>(true)
check<Equal<WritableOf<TestApp['strength']>, number | FloatValue>>(true)
check<Equal<WritableOf<TestApp['mode']>, 'OBJECT' | 'EDIT' | number>>(true)
check<Equal<WritableOf<TestApp['flags']>, readonly ('SELECT' | 'HIDE')[] | Int>>(true)
check<Equal<WritableOf<TestApp['items']>, never>>(true)
check<Equal<WritableOf<odd.Kinds['maybe']>, never>>(true)
check<Equal<WritableOf<odd.Kinds['new']>, Writable>>(true)
check<Equal<WritableOf<odd.Kinds['list']>, never>>(true)
check<Equal<WritableOf<At<odd.Kinds, 'v[0]'>>, never>>(true)
check<Equal<WritableOf<At<odd.Kinds, 'fixed[A]'>>, never>>(true)
check<Equal<ValueOf<At<odd.Kinds, 'fixed[A]'>>, boolean>>(true)
/* The reason is in the type of the parameter. */
check<Equal<At<TestApp, 'count.x'>['$no'], 'a value has no members'>>(true)
check<Equal<At<TestApp, 'items.weight'>['$no'] extends `a collection has ${string}` ? 1 : 2, 1>>(true)
check<Equal<At<TestApp, 'cuont'>['$no'], "no member 'cuont'">>(true)
type OddCommand = odd.Commands['a"\'b']
check<Equal<InputsOf<OddCommand>, {in?: Int; p?: RefTo<'a-b'> | null}>>(true)
check<Equal<OutputsOf<OddCommand>, {o: Int | undefined}>>(true)

/* ---- paths ---- */

declare const doc: Typed<Root, Commands>
declare const path: string
declare const item: RefTo<'TestItem'>
declare const other: RefTo<'Other'>
declare const ref: Ref
const is = <T>(_: T) => ({
  type: <U>(_same: Equal<T, U>) => {},
})

is(doc.get('count')).type<Int>(true)
is(doc.get('/count')).type<Int>(true)
is(doc.get('mode', {names: true})).type<'OBJECT' | 'EDIT' | number>(true)
is(doc.get('mode')).type<number>(true)
is(doc.get('mode[EDIT]')).type<boolean>(true)
is(doc.get('flags[HIDE]')).type<boolean>(true)
is(doc.get('focus[any]')).type<boolean>(true) /* dynamic */
is(doc.get('offset[1]')).type<number>(true)
is(doc.get('items.length')).type<Int>(true)
is(doc.get('items[0]')).type<RefTo<'TestItem'>>(true)
is(doc.get('items["a.b]"].weight')).type<number>(true)
is(doc.get("items['Cube'].name")).type<string>(true)
is(doc.get('items[#12].weight')).type<number>(true)
is(doc.get('items.active')).type<RefTo<'TestItem'>>(true)
is(doc.get('items.active.weight')).type<number>(true)
is(doc.get('activeItem.other.other.weight')).type<number>(true)
is(doc.get('commandDefaults["ts.add"].amount')).type<Int>(true)
is(doc.get(path)).type<Value>(true)
is(doc.get(path, {names: true})).type<NamedValue>(true)
check<Equal<At<Root, '/'>, TestApp>>(true)
/* Any of a family may have it, and an unknown struct anything. */
check<Equal<At<odd.Kinds, 'p.x'>, odd.Prop<'Int'> | odd.Prop<'String'>>>(true)
check<Equal<At<odd.Kinds, 'list[0].c'>, odd.Prop<'Int'>>>(true)
check<Equal<ValueOf<At<odd.Kinds, 'any.what[0].x'>>, NamedValue>>(true)

// @ts-expect-error: no member
doc.get('cuont')
// @ts-expect-error: no member
doc.get('')
// @ts-expect-error: no member
doc.get('count.')
// @ts-expect-error: no member of TestItem
doc.get('items[0].count')
// @ts-expect-error: not a member
doc.get('items[0].$struct')
// @ts-expect-error: a value has no members
doc.get('count.x')
// @ts-expect-error: no key
doc.get('mode[NOPE]')
// @ts-expect-error: not a key
doc.get('mode[0]')
// @ts-expect-error: not a key
doc.get('focus[0]')
// @ts-expect-error: a number
doc.get('offset[x]')
// @ts-expect-error: past the end of a path
doc.get('offset[0][1]')
// @ts-expect-error: an Int has no subscripts
doc.get('count[0]')
// @ts-expect-error: not of a collection
doc.get('items[KEY]')
// @ts-expect-error: not of a collection
doc.get('items.weight')
// @ts-expect-error: a pointer has no subscripts
doc.get('activeItem[0]')
// @ts-expect-error: a struct has no subscripts
doc.get('items[0][1]')
check<Equal<At<odd.Kinds, 'plain.active'> extends {$no: string} ? 1 : 2, 1>>(true)

doc.set('count', 3)
doc.set('count', 3n)
doc.set('mode', 'EDIT')
doc.set('flags', ['HIDE'])
doc.set('offset', [1, 2, 3])
doc.set('offset', new Float32Array(3))
doc.set('items[0].other', item)
doc.set('items[0].other', null)
doc.set('activeItem', ref)
doc.set('activeItem', doc.get('items[0]'), 7)
doc.set('commandDefaults["ts.add"].amount', 3)
doc.set(path, [true, false][0])
// @ts-expect-error: an Int
doc.set('count', 'x')
// @ts-expect-error: no such key
doc.set('mode', 'NOPE')
// @ts-expect-error: no such key
doc.set('flags', ['NOPE'])
// @ts-expect-error: 3 components
doc.set('offset', [1, 2])
// @ts-expect-error: a pointer to another struct
doc.set('activeItem', other)
// @ts-expect-error: read-only
doc.set('items.length', 1)
// @ts-expect-error: a collection
doc.set('items', null)
// @ts-expect-error: a struct
doc.set('items[0]', item)
// @ts-expect-error: no member
doc.set('cuont', 1)

const out = doc.run('ts.add', {amount: 2, default: undefined}, {prefix: '/items[0]'})
is(out).type<{result: Int}>(true)
doc.run('ts.add')
is(doc.canRun('ts.add', {amount: 1})).type<string | undefined>(true)
// @ts-expect-error: no command
doc.run('ts.nope')
// @ts-expect-error: no input
doc.run('ts.add', {amuont: 2})
// @ts-expect-error: an Int
doc.run('ts.add', {amount: 'x'})
// @ts-expect-error: no output
void out.reslut

/* The calls without types take what these give. */
void (() => {
  const untyped: Value = get(doc.ctx, 'activeItem')
  run(doc.ctx, 'ts.add', {item: doc.get('activeItem'), amount: doc.get('count')})
  return untyped
})
