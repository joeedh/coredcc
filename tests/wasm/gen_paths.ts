// Writes the test app's typed paths (gen-paths), and those of an odd catalog:
// node gen_paths.mjs <module.mjs> <out.ts> <odd.ts>
import {writeFileSync} from 'node:fs'
import {pathToFileURL} from 'node:url'

import * as cdcc from 'coredcc-script'
import {generatePaths} from 'coredcc-script/gen_paths.ts'
import type {Host} from 'coredcc-gen/testapp/Host'
import * as ta from 'coredcc-gen/testapp/functions'

const [modulePath, out, oddOut] = process.argv.slice(2)
const create = (await import(pathToFileURL(modulePath).href)).default
cdcc.load(await create(), 'TESTAPP_GetManager')
const host = ta.create() as Host
const session = new cdcc.Session(host, ta.context(host) as cdcc.Context)
cdcc.registerCommand(
  session.ctx,
  {id: 'ts.add', description: 'Adds */ to count', inputs: {amount: 1, default: ''}, outputs: {result: 0}},
  class {
    exec() {}
    undo() {}
  }
)
/* Names and texts that could break the output. */
const int = (name: string) => ({kind: 'Int', name, uiName: name})
const odd: cdcc.Catalog = {
  root: 'string',
  structs: [
    {name: 'string', uiName: '', description: 'ends */ here\u2028export const x = 1', members: [int('x')]},
    {name: 'a b', uiName: 'A', parent: 'string', members: [{...int('x'), kind: 'String'}, int('__proto__')]},
    {name: 'a-b', uiName: 'A', parent: 'a b', members: [int('constructor'), int('odd "name"\n')]},
    {name: '3d', uiName: '3D', parent: 'cycle', members: []},
    {name: 'cycle', uiName: 'C', parent: '3d', members: [int('c')]},
    {name: 'Structs', uiName: 'S', parent: 'nowhere', members: []},
    {name: 'Anystring_2', uiName: 'S', members: []},
    {
      name: 'Kinds',
      uiName: 'K',
      members: [
        {...int('e'), kind: 'Enum', items: [{key: 'a"|never', value: 0, uiName: ''}]},
        {...int('none'), kind: 'Enum'},
        {...int('open'), kind: 'Enum', items: [], dynamic: true},
        {...int('openFlags'), kind: 'Flags', items: [{key: 'A', value: 1, uiName: ''}], dynamic: true},
        {...int('f'), kind: 'Flags'},
        {...int('p'), kind: 'Pointer', target: 'string'},
        {...int('q'), kind: 'Pointer', targets: ['a-b', 'Kinds']},
        {...int('gone'), kind: 'Pointer', target: 'nowhere'},
        {...int('any'), kind: 'Pointer'},
        {...int('list'), kind: 'Collection', target: 'cycle', active: true},
        {...int('plain'), kind: 'Collection', target: 'Kinds', active: false},
        {...int('m'), kind: 'Mat4', readOnly: true},
        {...int('new'), kind: 'Newer'},
      ],
    },
  ],
  commands: [{id: 'a"\'b', uiName: '', flags: [], inputs: [int('in')], outputs: []}],
}
writeFileSync(oddOut, generatePaths(odd))
const text = generatePaths(cdcc.apiCatalog(session.ctx))
if (text !== generatePaths(cdcc.apiCatalog(session.ctx))) throw new Error('gen-paths: not deterministic')
writeFileSync(out, text)
session[Symbol.dispose]()
