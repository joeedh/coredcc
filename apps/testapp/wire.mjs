/**
 * Testbus reply encoding, shared by the page (bundled) and the Node-side client.
 *
 * CDP's `returnByValue` mangles ordinary values (bigint inside objects is an error,
 * NaN/-0/undefined are lost, typed arrays become index-keyed objects), so the page
 * returns a JSON *string* and the client decodes it. Non-JSON values are tagged as
 * `{"$t": <tag>, "v": <payload>}`; a real object that happens to own a `$t` key is
 * wrapped as `{"$t": "obj", "v": {...}}` so tags can't be forged by data.
 */

export const PROTOCOL_VERSION = 1

/** Replies longer than this many UTF-16 code units are refused (128 MB closed CDP). */
export const MAX_REPLY_CHARS = 16 * 1024 * 1024

const TYPED_ARRAYS = [
  'Int8Array',
  'Uint8Array',
  'Uint8ClampedArray',
  'Int16Array',
  'Uint16Array',
  'Int32Array',
  'Uint32Array',
  'Float32Array',
  'Float64Array',
  'BigInt64Array',
  'BigUint64Array',
]

function tagNumber(n) {
  if (Number.isNaN(n)) return {$t: 'num', v: 'NaN'}
  if (n === Infinity) return {$t: 'num', v: 'Infinity'}
  if (n === -Infinity) return {$t: 'num', v: '-Infinity'}
  if (Object.is(n, -0)) return {$t: 'num', v: '-0'}
  return n
}

/** Convert a value into a JSON-safe tree. Throws on cycles. */
function toWire(value, seen) {
  switch (typeof value) {
    case 'undefined':
      return {$t: 'undef'}
    case 'bigint':
      return {$t: 'bigint', v: value.toString()}
    case 'number':
      return tagNumber(value)
    case 'string':
    case 'boolean':
      return value
    case 'function':
    case 'symbol':
      return {$t: 'undef'}
  }
  if (value === null) return null
  if (seen.has(value)) throw new TypeError('testbus: cyclic value cannot be encoded')
  seen.add(value)
  try {
    if (ArrayBuffer.isView(value) && !(value instanceof DataView)) {
      const name = value.constructor.name
      if (!TYPED_ARRAYS.includes(name)) throw new TypeError(`testbus: unknown typed array ${name}`)
      return {$t: 'typed', k: name, v: Array.from(value, (x) => toWire(x, seen))}
    }
    if (Array.isArray(value)) return value.map((x) => toWire(x, seen))
    if (value instanceof Map) return {$t: 'map', v: [...value].map((kv) => toWire(kv, seen))}
    if (value instanceof Set) return {$t: 'set', v: [...value].map((x) => toWire(x, seen))}
    if (value instanceof Date) return {$t: 'date', v: value.toISOString()}
    if (value instanceof Error) {
      return {$t: 'error', v: {name: value.name, message: value.message, stack: value.stack}}
    }
    const out = {}
    for (const k of Object.keys(value)) out[k] = toWire(value[k], seen)
    return Object.prototype.hasOwnProperty.call(out, '$t') ? {$t: 'obj', v: out} : out
  } finally {
    seen.delete(value)
  }
}

function fromWire(node) {
  if (node === null || typeof node !== 'object') return node
  if (Array.isArray(node)) return node.map(fromWire)
  if (!Object.prototype.hasOwnProperty.call(node, '$t')) {
    const out = {}
    for (const k of Object.keys(node)) out[k] = fromWire(node[k])
    return out
  }
  switch (node.$t) {
    case 'undef':
      return undefined
    case 'bigint':
      return BigInt(node.v)
    case 'num':
      return node.v === '-0' ? -0 : Number(node.v)
    case 'typed':
      return new globalThis[node.k](node.v.map(fromWire))
    case 'map':
      return new Map(node.v.map(fromWire))
    case 'set':
      return new Set(node.v.map(fromWire))
    case 'date':
      return new Date(node.v)
    case 'error': {
      const e = new Error(node.v.message)
      e.name = node.v.name
      e.stack = node.v.stack
      return e
    }
    case 'obj': {
      const out = {}
      for (const k of Object.keys(node.v)) out[k] = fromWire(node.v[k])
      return out
    }
    default:
      throw new TypeError(`testbus: unknown wire tag ${node.$t}`)
  }
}

/** Serialize an error into a plain reply error. */
export function errorInfo(err) {
  if (err instanceof Error) return {name: err.name, message: err.message, stack: err.stack}
  return {name: 'Error', message: String(err)}
}

/** Encode a reply object (`{ok, value}` / `{ok, error}`) to a string, enforcing the cap. */
export function encodeReply(reply) {
  let text
  try {
    text = JSON.stringify(toWire(reply, new Set()))
  } catch (err) {
    return JSON.stringify({ok: false, error: errorInfo(err)})
  }
  if (text.length > MAX_REPLY_CHARS) {
    return JSON.stringify({
      ok: false,
      error: {
        name: 'ReplyTooLarge',
        message: `reply is ${text.length} chars, over the ${MAX_REPLY_CHARS} cap; use outPath`,
      },
    })
  }
  return text
}

/** Decode a reply string produced by `encodeReply`. */
export function decodeReply(text) {
  return fromWire(JSON.parse(text))
}
