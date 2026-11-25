/**
 * get, set and run with the types that gen-paths makes (gen_paths.ts): paths, values, command ids
 * and inputs are checked by TypeScript. At run time these are the functions of index.ts and run.ts.
 *
 *     import type {Commands, Root} from './paths.ts' // generated
 *     const doc = typed<Root, Commands>(ctx)
 *     doc.set('objects["Cube"].location[1]', 2)
 *
 * A path that isn't a literal is not checked: its value is a NamedValue, and a Writable to set.
 */
import type {Context} from 'coredcc-gen/coredcc/Context'

import type {FloatValue, NamedValue, Ref, Value, Writable} from './core.ts'
import {get, set} from './index.ts'
import {canRun, run, type RunOptions} from './run.ts'

/* ---- what gen-paths makes ---- */

/** A member, an input or an output: what a generated `Prop` is. */
export interface AnyProp {
  readonly $kind: string
  readonly $keys: string
  readonly $target: unknown
  readonly $is: string
}

/** A pointer to a struct of this name: what a Ref is, as far as TypeScript knows. */
export type RefTo<Struct extends string = string> = Ref & {readonly $struct?: Struct}

type Int = number | bigint
type RefOf<T> = RefTo<T extends {readonly $struct: infer S extends string} ? S : string>
type Vectors = {Vec2: [number, number]; Vec3: [number, number, number]; Vec4: [number, number, number, number]}
type Plain = Vectors & {
  Bool: boolean
  Int: Int
  Float: number
  String: string
  Quat: Vectors['Vec4']
  Mat4: number[]
}
type Named<M, Names extends boolean> = M extends AnyProp
  ? M['$kind'] extends keyof Plain
    ? Plain[M['$kind']]
    : M['$kind'] extends 'Enum'
      ? (Names extends true ? M['$keys'] : never) | number
      : M['$kind'] extends 'Flags'
        ? (Names extends true ? M['$keys'][] : never) | Int
        : M['$kind'] extends 'Pointer'
          ? RefOf<M['$target']> | null
          : M['$kind'] extends 'Collection'
            ? never
            : NamedValue
  : RefOf<M>
type Maybe<M> = M extends AnyProp ? ('Optional' extends M['$is'] ? undefined : never) : never

/** The value of a member as `get` gives it: `ValueOf<TestApp['mode']>`. */
export type ValueOf<M> = Named<M, false> | Maybe<M>
/** With `{names: true}`. A number is one that no item has: see `toNamed`. */
export type NamedOf<M> = Named<M, true> | Maybe<M>
/** What can be written to it. Nothing, for a Mat4, a collection or a struct. */
export type WritableOf<M> = M extends AnyProp
  ? 'ReadOnly' extends M['$is']
    ? never
    : M['$kind'] extends 'Float'
      ? number | FloatValue
      : M['$kind'] extends 'Enum'
        ? M['$keys'] | number
        : M['$kind'] extends 'Flags'
          ? readonly M['$keys'][] | Int
          : M['$kind'] extends 'Vec2' | 'Vec3' | 'Vec4' | 'Quat'
            ? Readonly<Plain[M['$kind'] & keyof Plain]> | Float32Array | Float64Array
            : M['$kind'] extends 'Pointer'
              ? RefOf<M['$target']> | null
              : M['$kind'] extends 'Mat4' | 'Collection'
                ? never
                : M['$kind'] extends keyof Plain
                  ? Plain[M['$kind']]
                  : Writable
  : never

/** The inputs of a command, none of which have to be given. */
export type InputsOf<C> = C extends {inputs: infer I} ? {[K in keyof I]?: WritableOf<I[K]>} : never
/** Its outputs, as `run` returns them. */
export type OutputsOf<C> = C extends {outputs: infer O} ? {[K in keyof O]: ValueOf<O[K]>} : never

/* ---- paths ---- */

/* A prop made here: `.length`, a component, an enum's key, or `Any`, which isn't known. */
interface Made<Kind extends string, Is extends string = never> {
  readonly $kind: Kind
  readonly $keys: never
  readonly $target: never
  readonly $is: Is
}
type NoPath<Why extends string> = {readonly $no: Why}

/* The first name of a path, and the rest: `''`, or from its `[` or its `.`. */
type Head<P extends string> = P extends `${infer A}.${infer B}`
  ? A extends `${infer C}[${infer D}`
    ? [C, `[${D}.${B}`]
    : [A, `.${B}`]
  : P extends `${infer C}[${infer D}`
    ? [C, `[${D}`]
    : [P, '']

/* A subscript, and what follows it. */
type Bracket<P extends string> = P extends `["${infer K}"]${infer R}`
  ? [K, R, 'key']
  : P extends `['${infer K}']${infer R}`
    ? [K, R, 'key']
    : P extends `[${infer K}]${infer R}`
      ? K extends `#${bigint}`
        ? [K, R, 'handle']
        : K extends `${bigint}`
          ? [K, R, 'index']
          : [K, R, 'name']
      : undefined

/* What a struct (or any of several) has by the name. */
type MemberOf<T, N extends string> = T extends unknown ? (N extends keyof T ? (N extends `$${string}` ? never : T[N]) : never) : never

type Name<T, P extends string> =
  Head<P> extends [infer N extends string, infer R extends string]
    ? [MemberOf<T, N>] extends [never]
      ? NoPath<`no member '${N}'`>
      : After<MemberOf<T, N>, R>
    : never

/* From `T`, a prop or a struct, along the rest of the path. */
type After<T, R extends string> = R extends ''
  ? T
  : unknown extends T
    ? Made<'Any'>
    : T extends AnyProp
    ? T['$kind'] extends 'Collection'
      ? Items<T, R>
      : T['$kind'] extends 'Pointer'
        ? R extends `.${infer N}`
          ? After<T['$target'], `.${N}`>
          : NoPath<'a pointer has no subscripts'>
        : Bracket<R> extends [infer K extends string, infer Rest extends string, infer Form]
          ? Rest extends ''
            ? Sub<T, K, Form>
            : NoPath<`nothing is past '[${K}]'`>
          : NoPath<'a value has no members'>
    : R extends `.${infer N}`
      ? Name<T, N>
      : NoPath<'a struct has no subscripts'>

type Items<T extends AnyProp, R extends string> = R extends '.length'
  ? Made<'Int', 'ReadOnly'>
  : R extends `.active${infer More}`
    ? 'Active' extends T['$is']
      ? After<T['$target'], More>
      : NoPath<'the collection has no active item'>
    : Bracket<R> extends [string, infer Rest extends string, 'key' | 'index' | 'handle']
      ? After<T['$target'], Rest>
      : NoPath<'a collection has length, active, and items by a key, a position or a handle'>

type Sub<T extends AnyProp, K extends string, Form> = T['$kind'] extends 'Enum' | 'Flags'
  ? Form extends 'name'
    ? K extends T['$keys']
      ? Made<'Bool', Extract<T['$is'], 'ReadOnly'>>
      : NoPath<`no key '${K}'`>
    : NoPath<'an enum has subscripts of keys'>
  : T['$kind'] extends 'Vec2' | 'Vec3' | 'Vec4' | 'Quat' | 'Mat4'
    ? Form extends 'index'
      ? Made<'Float', Extract<T['$is'], 'ReadOnly'>>
      : NoPath<'a vector has subscripts of numbers'>
    : NoPath<'it has no subscripts'>

/** What a path names: a Prop, a struct, or a NoPath with the reason. */
export type At<Root, P extends string> = P extends '/' ? Root : P extends `/${infer R}` ? Name<Root, R> : Name<Root, P>

/* What it may name: of a struct that others inherit from, any of them may have it. */
type Found<Root, P extends string> = Exclude<At<Root, P>, NoPath<string>>
/* The parameter that takes the path `P`: itself, or if it is none, a text that says why. */
type PathArg<Root, P extends string> = string extends P
  ? P
  : [Found<Root, P>] extends [never]
    ? Extract<At<Root, P>, NoPath<string>>['$no']
    : P
type Read<Root, P extends string, Names extends boolean> = string extends P
  ? Names extends true
    ? NamedValue
    : Value
  : Names extends true
    ? NamedOf<Found<Root, P>>
    : ValueOf<Found<Root, P>>
type Write<Root, P extends string> = string extends P ? Writable : WritableOf<Found<Root, P>>

/* ---- the calls ---- */

export interface Typed<Root, Commands> {
  readonly ctx: Context
  get<P extends string, Names extends boolean = false>(
    path: PathArg<Root, P>,
    opts?: {names?: Names}
  ): Read<Root, P, Names>
  set<P extends string>(path: PathArg<Root, P>, value: Write<Root, P>, gesture?: number): void
  run<C extends keyof Commands & string>(
    id: C,
    inputs?: InputsOf<Commands[C]>,
    opts?: RunOptions
  ): OutputsOf<Commands[C]>
  canRun<C extends keyof Commands & string>(
    id: C,
    inputs?: InputsOf<Commands[C]>,
    opts?: RunOptions
  ): string | undefined
}

/** `ctx`'s calls, with the types of its root and of the commands. */
export function typed<Root, Commands>(ctx: Context): Typed<Root, Commands> {
  const calls = {
    ctx,
    get: (path: string, opts?: {names?: boolean}) => get(ctx, path, opts as {names: true}),
    set: (path: string, value: Writable, gesture?: number) => set(ctx, path, value, gesture),
    run: (id: string, inputs?: Record<string, Writable | undefined>, opts?: RunOptions) => run(ctx, id, inputs, opts),
    canRun: (id: string, inputs?: Record<string, Writable | undefined>, opts?: RunOptions) =>
      canRun(ctx, id, inputs, opts),
  }
  return calls as Typed<Root, Commands>
}
