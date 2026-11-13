import { Buffer } from 'node:buffer';
import { types } from 'node:util';
import * as profile from '@zlink-systems/stream-wire/internal/json';

export const FRAMEWORK_JSON_V1_PROFILE = profile.FRAMEWORK_JSON_V1_PROFILE;
export const stringifyFrameworkJsonV1 = profile.createFrameworkJsonV1Stringifier(
  encodeNodeBase64,
  prepareNodeJsonBytes
);

function encodeNodeBase64(bytes: Uint8Array): string {
  // Borrow the exact byte range. Native Base64 avoids a spread argument list
  // and an intermediate binary string without copying the input storage.
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('base64');
}

const DEFAULT_PARSE_OPTIONS = Object.freeze({});

export function parseFrameworkJsonV1(
  text: string,
  options: profile.FrameworkJsonParseOptions = DEFAULT_PARSE_OPTIONS,
  schema?: Parameters<typeof profile.parseFrameworkJsonV1>[2],
  reviver?: Parameters<typeof profile.parseFrameworkJsonV1>[3]
): unknown {
  return profile.parseFrameworkJsonV1(text, options, schema, reviver, decodeNodeBase64);
}

function decodeNodeBase64(value: string): Uint8Array {
  const bytes = Buffer.from(value, 'base64');
  return new Uint8Array(bytes.buffer, bytes.byteOffset, bytes.byteLength);
}

// Only data graphs are projected. Observable accessors, proxies, toJSON methods
// and cycles retain the original stringify path and its reads.
const UNPREPARED = Symbol('unprepared JSON graph');
const BUFFER_TO_JSON = Buffer.prototype.toJSON;

function prepareNodeJsonBytes(value: unknown, convert: (value: unknown) => unknown): unknown {
  const ancestors = new Set<object>();
  const prepared = visit(value);
  return prepared === UNPREPARED ? undefined : prepared;

  function visit(source: unknown): unknown {
    if (typeof source === 'bigint' || typeof source === 'function') return UNPREPARED;
    if (typeof source !== 'object' || source === null) return convert(source);
    if (types.isProxy(source) || types.isBoxedPrimitive(source)) return UNPREPARED;
    const prototype: unknown = Object.getPrototypeOf(source);
    const bytes = source instanceof Uint8Array;
    if (bytes) {
      if (prototype !== Buffer.prototype && prototype !== Uint8Array.prototype) return UNPREPARED;
    }
    for (let owner: object | null = source; owner !== null;) {
      if (types.isProxy(owner)) return UNPREPARED;
      const descriptor = Object.getOwnPropertyDescriptor(owner, 'toJSON');
      if (descriptor !== undefined) {
        if (!('value' in descriptor)) return UNPREPARED;
        if (
          typeof descriptor.value === 'function' &&
          !(bytes && descriptor.value === BUFFER_TO_JSON)
        ) {
          return UNPREPARED;
        }
        break;
      }
      owner = Object.getPrototypeOf(owner) as object | null;
    }
    if (bytes) {
      // These names are read by the native byte primitive or Buffer.toJSON.
      // An own override can make omitting toJSON observable.
      for (const name of ['length', 'buffer', 'byteOffset', 'byteLength']) {
        if (Object.hasOwn(source, name) || Object.hasOwn(prototype as object, name)) {
          return UNPREPARED;
        }
      }
      return convert(source);
    }
    convert(source);
    if (ancestors.has(source)) return UNPREPARED;
    ancestors.add(source);
    const descriptors = Object.getOwnPropertyDescriptors(source);
    const names = Array.isArray(source)
      ? Array.from({ length: source.length }, (_, index) => String(index))
      : Object.keys(source);
    let changed = false;
    for (const name of names) {
      const descriptor = descriptors[name];
      if (!Object.hasOwn(descriptors, name) || !('value' in descriptor)) return UNPREPARED;
      const item = visit(descriptor.value);
      if (item === UNPREPARED) return UNPREPARED;
      if (!Object.is(item, descriptor.value)) {
        descriptor.value = item;
        changed = true;
      }
    }
    ancestors.delete(source);
    if (!changed) return source;
    return Array.isArray(source)
      ? Object.defineProperties([], descriptors)
      : Object.create(prototype as object | null, descriptors);
  }
}

export type { FrameworkJsonParseOptions } from '@zlink-systems/stream-wire/internal/json';
