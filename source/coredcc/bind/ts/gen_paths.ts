/**
 * gen-paths: TypeScript types of a document's data and commands, from `apiCatalog`.
 *
 * - One interface per struct, with all its members, the inherited ones too. A member's type is
 *   what `get(path, {names: true})` gives, but a pointer's is the interface of what it points
 *   to. There is no union of all paths, which would be infinite.
 * - `Structs`, the interfaces by the structs' names, and `Root`, the context root's.
 * - `Commands`, the inputs and outputs of each command by its id, and `CommandId`.
 *
 * Types only: a host writes the text to a file of its own.
 */
import type {Catalog, CatalogMember, CatalogStruct} from './catalog.ts'

/* What the generated names can't be: keywords, the built-in types, and the names it makes. */
const TAKEN = [
  'any', 'as', 'async', 'await', 'bigint', 'boolean', 'break', 'case', 'catch', 'class', 'const', 'constructor',
  'continue', 'debugger', 'declare', 'default', 'delete', 'do', 'else', 'enum', 'export', 'extends', 'false',
  'finally', 'for', 'from', 'function', 'get', 'if', 'implements', 'import', 'in', 'infer', 'instanceof',
  'interface', 'is', 'keyof', 'let', 'module', 'namespace', 'never', 'new', 'null', 'number', 'object', 'of',
  'package', 'private', 'protected', 'public', 'readonly', 'require', 'return', 'set', 'static', 'string', 'super',
  'switch', 'symbol', 'this', 'throw', 'true', 'try', 'type', 'typeof', 'undefined', 'unique', 'unknown', 'var',
  'void', 'while', 'with', 'yield', 'Collection', 'Commands', 'CommandId', 'Pointer', 'Root', 'Structs',
]

const quoted = (s: string) => JSON.stringify(s)
const key = (s: string) => (/^[A-Za-z_$][\w$]*$/.test(s) ? s : quoted(s))

/* A doc comment of the lines that aren't empty; "" for none. */
function comment(lines: (string | undefined)[], indent = ''): string {
  const text = lines
    .filter((l) => l)
    .join('\n')
    .replace(/\*\//g, '*\\/')
    .split(/[\r\n]+/)
  if (text.length === 1 && text[0] === '') return ''
  if (text.length === 1) return `${indent}/** ${text[0]} */\n`
  return `${indent}/**\n${text.map((l) => `${indent} * ${l}\n`).join('')}${indent} */\n`
}

/** The TypeScript module for `catalog`. The same text for the same catalog. */
export function generatePaths(catalog: Catalog): string {
  const structs = new Map(catalog.structs.map((s) => [s.name, s]))
  const taken = new Set(TAKEN)
  const identifier = (name: string) => {
    let base = name.replace(/[^\w$]+/g, '_')
    if (!/^[A-Za-z_$]/.test(base)) base = `_${base}`
    let id = base
    for (let i = 2; taken.has(id); i++) id = `${base}_${i}`
    taken.add(id)
    return id
  }
  const names = new Map(catalog.structs.map((s) => [s.name, identifier(s.name)]))
  const children = new Map<string, string[]>()
  for (const s of catalog.structs) {
    if (s.parent !== undefined && structs.has(s.parent)) {
      children.set(s.parent, [...(children.get(s.parent) ?? []), s.name])
    }
  }
  /* The struct and those that inherit from it. A cycle of parents ends. */
  const family = (name: string) => {
    const out = new Set([name])
    for (const s of out) for (const c of children.get(s) ?? []) out.add(c)
    return [...out]
  }
  /* A struct that has others inherit from it has a union of them all. */
  const unions = new Map<string, string>()
  for (const s of catalog.structs) if (children.has(s.name)) unions.set(s.name, identifier(`Any${names.get(s.name)}`))
  const reference = (name: string | undefined) =>
    name === undefined ? 'unknown' : (unions.get(name) ?? names.get(name) ?? 'unknown')

  const typeOf = (m: CatalogMember): string => {
    const items = m.items?.map((i) => quoted(i.key)).join(' | ')
    switch (m.kind) {
      case 'Bool':
        return 'boolean'
      case 'Int':
        return 'number | bigint'
      case 'Float':
        return 'number'
      case 'String':
        return 'string'
      case 'Enum':
        return items || 'number'
      case 'Flags':
        return items ? `(${items})[]` : 'number | bigint'
      case 'Vec2':
        return '[number, number]'
      case 'Vec3':
        return '[number, number, number]'
      case 'Vec4':
      case 'Quat':
        return '[number, number, number, number]'
      case 'Mat4':
        return 'number[]'
      case 'Pointer': {
        const targets = m.targets ?? (m.target === undefined ? [] : [m.target])
        const types = [...new Set(targets.map(reference))]
        return types.length === 0 ? 'unknown' : `${types.join(' | ')} | null`
      }
      case 'Collection':
        return `Collection<${reference(m.target)}${m.active ? '' : ', never'}>`
      default:
        return 'unknown'
    }
  }
  const property = (m: CatalogMember, indent: string, optional = false) =>
    comment([m.description], indent) +
    `${indent}${m.readOnly ? 'readonly ' : ''}${key(m.name)}${optional ? '?' : ''}: ${typeOf(m)}\n`

  /* With the inherited members, which a struct's own ones of the same name hide. */
  const members = (s: CatalogStruct) => {
    const out = new Map<string, CatalogMember>()
    const seen = new Set<string>()
    for (let at: CatalogStruct | undefined = s; at && !seen.has(at.name); ) {
      seen.add(at.name)
      for (const m of at.members) if (!out.has(m.name)) out.set(m.name, m)
      at = at.parent === undefined ? undefined : structs.get(at.parent)
    }
    return [...out.values()]
  }

  let out = '/* Generated by coredcc gen-paths. */\n\n'
  out += '/** Items by position. `active` is for those that have an active one. */\n'
  out += 'export interface Collection<T, Active = T | null> {\n'
  out += '  readonly length: number\n  readonly active: Active\n  readonly [index: number]: T\n}\n'
  for (const s of catalog.structs) {
    out += '\n' + comment([s.description ?? s.uiName])
    out += `export interface ${names.get(s.name)} {\n`
    for (const m of members(s)) out += property(m, '  ')
    out += '}\n'
    const union = unions.get(s.name)
    if (union) {
      out += `export type ${union} = ${family(s.name).map((n) => names.get(n)).join(' | ')}\n`
    }
  }
  out += '\n/** The structs, by name. */\nexport interface Structs {\n'
  for (const s of catalog.structs) out += `  ${quoted(s.name)}: ${names.get(s.name)}\n`
  out += '}\n'
  out += `\n/** The context root. */\nexport type Root = ${(catalog.root !== undefined && names.get(catalog.root)) || 'unknown'}\n`
  out += '\n/** The commands, by id. */\nexport interface Commands {\n'
  for (const c of catalog.commands) {
    out += comment([c.description], '  ')
    out += `  ${quoted(c.id)}: {\n    inputs: {\n`
    for (const m of c.inputs) out += property(m, '      ', true)
    out += '    }\n    outputs: {\n'
    for (const m of c.outputs) out += property(m, '      ')
    out += '    }\n  }\n'
  }
  out += '}\nexport type CommandId = keyof Commands\n'
  return out
}
