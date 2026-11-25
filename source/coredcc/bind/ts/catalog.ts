/**
 * What is registered, for generators (gen_paths.ts).
 */
import type {Context} from 'coredcc-gen/coredcc/Context'
import * as cd from 'coredcc-gen/coredcc/functions'

import {take} from './core.ts'

/** A member, an input or an output: bind/catalog.h, with the fields of `meta` (PathMeta). */
export interface CatalogMember {
  kind: string
  name: string
  uiName: string
  description?: string
  readOnly?: boolean
  /** Of a Pointer or a Collection. */
  target?: string
  targets?: string[]
  active?: boolean
  items?: {key: string; value: number | string; uiName: string}[]
}
export interface CatalogStruct {
  name: string
  uiName: string
  description?: string
  parent?: string
  /** Its own. */
  members: CatalogMember[]
}
export interface CatalogCommand {
  id: string
  uiName: string
  description?: string
  icon?: string
  flags: string[]
  inputs: CatalogMember[]
  outputs: CatalogMember[]
}
export interface Catalog {
  /** The struct of the context's root. */
  root?: string
  structs: CatalogStruct[]
  commands: CatalogCommand[]
}

/** The structs and commands that are registered. */
export function apiCatalog(ctx: Context): Catalog {
  return JSON.parse(take(cd.apiCatalog(ctx), (json) => json)) as Catalog
}
