/**
 * What gen-paths makes of the test app's structs and commands, checked by tsc (run.mjs): a wrong
 * type fails, and so does a `@ts-expect-error` without an error.
 */
import type {Collection, CommandId, Commands, Root, Structs, TestApp, TestItem} from 'coredcc-paths'
import type * as odd from 'coredcc-odd-paths'

type Equal<A, B> = (<T>() => T extends A ? 1 : 2) extends <T>() => T extends B ? 1 : 2 ? true : false
const check = <T extends true>(_: T) => {}

check<Equal<Root, TestApp>>(true)
check<Equal<Structs['TestItem'], TestItem>>(true)
check<Equal<TestApp['count'], number | bigint>>(true)
check<Equal<TestApp['strength'], number>>(true)
check<Equal<TestApp['enabled'], boolean>>(true)
check<Equal<TestApp['label'], string>>(true)
check<Equal<TestApp['offset'], [number, number, number]>>(true)
check<Equal<TestApp['mode'], 'OBJECT' | 'EDIT'>>(true)
check<Equal<TestApp['focus'], (string & {}) | 'NONE'>>(true) /* dynamic */
check<Equal<TestApp['flags'], ('SELECT' | 'HIDE')[]>>(true)
check<Equal<TestApp['activeItem'], TestItem | null>>(true)
check<Equal<TestApp['items'], Collection<TestItem>>>(true)
/* Through references, however far. */
check<Equal<NonNullable<NonNullable<TestItem['other']>['other']>['weight'], number>>(true)
check<Equal<TestApp['items'][0]['name'], string>>(true)

check<Equal<Commands['ts.add']['inputs'], {amount?: number | bigint; default?: string}>>(true)
check<Equal<Commands['ts.add']['outputs'], {result: number | bigint}>>(true)
const id: CommandId = 'ts.add'
void id
// @ts-expect-error: no such command
const none: CommandId = 'ts.nope'
void none

/* The odd catalog's (gen_paths.ts): `string` is a struct there, as `string_2`. */
check<Equal<odd.Root, odd.string_2>>(true)
check<Equal<odd.string_2, {x: number | bigint}>>(true)
check<Equal<odd.a_b['x'], string>>(true) /* its own hides the inherited one */
check<Equal<odd.a_b_2['x'], string>>(true)
check<Equal<keyof odd.a_b_2, 'x' | '__proto__' | 'constructor' | 'odd "name"\n'>>(true)
check<Equal<odd._3d, {c: number | bigint}>>(true)
check<Equal<odd.Anystring_2_2, odd.string_2 | odd.a_b | odd.a_b_2>>(true)
check<Equal<odd.Kinds['e'], 'a"|never'>>(true)
check<Equal<odd.Kinds['none'], number>>(true)
check<Equal<odd.Kinds['open'], string>>(true)
check<Equal<odd.Kinds['openFlags'], ((string & {}) | 'A')[]>>(true)
check<Equal<odd.Kinds['f'], number | bigint>>(true)
check<Equal<odd.Kinds['p'], odd.Anystring_2_2 | null>>(true)
check<Equal<odd.Kinds['q'], odd.a_b_2 | odd.Kinds | null>>(true)
check<Equal<odd.Kinds['gone'], unknown>>(true)
check<Equal<odd.Kinds['list']['active'], odd.Anycycle | null>>(true)
check<Equal<odd.Kinds['plain']['active'], never>>(true)
check<Equal<odd.Kinds['any'], unknown>>(true)
check<Equal<Pick<odd.Kinds, 'f' | 'm'>, {f: number | bigint; readonly m: number[]}>>(true)
check<Equal<odd.Kinds['new'], unknown>>(true)
check<Equal<odd.CommandId, 'a"\'b'>>(true)

declare const app: TestApp
app.count = 3
// @ts-expect-error: no such member
void app.cuont
// @ts-expect-error: a collection can't be replaced
app.items = app.items
