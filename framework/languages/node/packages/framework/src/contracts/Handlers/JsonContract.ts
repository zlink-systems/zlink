/** Describes the fields accepted by the default framework-json-v1 serializer. */
export type ZLinkJsonSchema =
  | { readonly type: 'boolean' }
  | { readonly type: 'string' }
  | { readonly type: 'number' }
  | { readonly type: 'int32' }
  | { readonly type: 'uint32' }
  | { readonly type: 'int64' }
  | { readonly type: 'uint64' }
  | { readonly type: 'bytes' }
  | { readonly type: 'enum'; readonly names: readonly string[] }
  | { readonly type: 'nullable'; readonly value: ZLinkJsonSchema }
  | { readonly type: 'array'; readonly items: ZLinkJsonSchema }
  | { readonly type: 'record'; readonly values: ZLinkJsonSchema }
  | {
      readonly type: 'object';
      readonly properties: Readonly<Record<string, ZLinkJsonSchema>>;
      readonly required: readonly string[];
      /** Unknown properties are ignored during decoding unless this is false. */
      readonly additionalProperties?: boolean;
    };

/**
 * Describes the request and optional reply fields carried under one packet
 * name by the default JSON serializer.
 */
export interface ZLinkPacketJsonContract {
  readonly payload: ZLinkJsonSchema;
  readonly reply?: ZLinkJsonSchema;
}

const ZLINK_PACKET_JSON_CONTRACTS = Symbol.for('@zlink-systems/framework:packet-json-contracts');

export function defineZLinkPacketJsonContract(
  packetName: string,
  contract: ZLinkPacketJsonContract
): ZLinkPacketJsonContract {
  if (packetName.trim().length === 0) {
    throw new TypeError('ZLink packet name must not be empty.');
  }
  if (typeof contract !== 'object' || contract === null) {
    throw new TypeError('ZLink packet JSON contract must be an object.');
  }
  const normalized = Object.freeze({
    payload: normalizeJsonSchema(contract.payload, new Set(), 0),
    ...(contract.reply === undefined
      ? {}
      : { reply: normalizeJsonSchema(contract.reply, new Set(), 0) })
  });
  const contracts = packetJsonContracts();
  const current = contracts.get(packetName);
  if (current !== undefined && canonicalContract(current) !== canonicalContract(normalized)) {
    throw new TypeError(`ZLink packet '${packetName}' has conflicting JSON contracts in one process.`);
  }
  if (current !== undefined) return current;
  contracts.set(packetName, normalized);
  return normalized;
}

export function readZLinkPacketJsonContract(packetName: string): ZLinkPacketJsonContract | undefined {
  return packetJsonContracts().get(packetName);
}

function normalizeJsonSchema(
  schema: ZLinkJsonSchema,
  parents: Set<object>,
  depth: number
): ZLinkJsonSchema {
  if (typeof schema !== 'object' || schema === null || depth > 64) {
    throw new TypeError('ZLink packet JSON schema must be an acyclic object with at most 64 levels.');
  }
  if (parents.has(schema)) throw new TypeError('ZLink packet JSON schema must not contain a cycle.');
  const nextParents = new Set(parents).add(schema);
  switch (schema.type) {
    case 'boolean':
    case 'string':
    case 'number':
    case 'int32':
    case 'uint32':
    case 'int64':
    case 'uint64':
    case 'bytes':
      return Object.freeze({ type: schema.type });
    case 'enum': {
      if (!Array.isArray(schema.names)
          || schema.names.length === 0
          || schema.names.some((name) => typeof name !== 'string' || name.length === 0)) {
        throw new TypeError('ZLink packet enum schema requires non-empty string names.');
      }
      if (new Set(schema.names).size !== schema.names.length) {
        throw new TypeError('ZLink packet enum schema names must be unique.');
      }
      return Object.freeze({ type: 'enum', names: Object.freeze([...schema.names]) });
    }
    case 'nullable':
      return Object.freeze({ type: 'nullable', value: normalizeJsonSchema(schema.value, nextParents, depth + 1) });
    case 'array':
      return Object.freeze({ type: 'array', items: normalizeJsonSchema(schema.items, nextParents, depth + 1) });
    case 'record':
      return Object.freeze({ type: 'record', values: normalizeJsonSchema(schema.values, nextParents, depth + 1) });
    case 'object': {
      if (typeof schema.properties !== 'object' || schema.properties === null || Array.isArray(schema.properties)) {
        throw new TypeError('ZLink packet object schema properties must be an object.');
      }
      if (!Array.isArray(schema.required)
          || schema.required.some((name) => typeof name !== 'string' || name.length === 0)) {
        throw new TypeError('ZLink packet object schema required must contain property names.');
      }
      if (schema.additionalProperties !== undefined && typeof schema.additionalProperties !== 'boolean') {
        throw new TypeError('ZLink packet object schema additionalProperties must be boolean.');
      }
      const properties: Record<string, ZLinkJsonSchema> = {};
      for (const [name, property] of Object.entries(schema.properties)) {
        if (name === '__proto__' || name === 'prototype' || name === 'constructor') {
          throw new TypeError(`ZLink packet JSON property '${name}' is not allowed.`);
        }
        properties[name] = normalizeJsonSchema(property, nextParents, depth + 1);
      }
      if (new Set(schema.required).size !== schema.required.length) {
        throw new TypeError('ZLink packet JSON required properties must be unique.');
      }
      for (const name of schema.required) {
        if (name === '__proto__' || name === 'prototype' || name === 'constructor') {
          throw new TypeError(`ZLink packet JSON property '${name}' is not allowed.`);
        }
        if (!Object.prototype.hasOwnProperty.call(properties, name)) {
          throw new TypeError(`ZLink packet JSON required property '${name}' has no property schema.`);
        }
      }
      return Object.freeze({
        type: 'object',
        properties: Object.freeze(properties),
        required: Object.freeze([...schema.required]),
        ...(schema.additionalProperties === undefined
          ? {}
          : { additionalProperties: schema.additionalProperties })
      });
    }
  }
  throw new TypeError(`ZLink packet JSON schema type '${String((schema as { type?: unknown }).type)}' is not supported.`);
}

function packetJsonContracts(): Map<string, ZLinkPacketJsonContract> {
  const root = globalThis as typeof globalThis & Record<symbol, unknown>;
  let contracts = root[ZLINK_PACKET_JSON_CONTRACTS] as Map<string, ZLinkPacketJsonContract> | undefined;
  if (contracts === undefined) {
    contracts = new Map();
    Object.defineProperty(root, ZLINK_PACKET_JSON_CONTRACTS, {
      configurable: false,
      enumerable: false,
      value: contracts,
      writable: false
    });
  }
  return contracts;
}

function canonicalContract(value: unknown): string {
  if (Array.isArray(value)) return `[${value.map(canonicalContract).join(',')}]`;
  if (typeof value !== 'object' || value === null) return JSON.stringify(value);
  return `{${Object.entries(value as Record<string, unknown>)
    .sort(([left], [right]) => left < right ? -1 : left > right ? 1 : 0)
    .map(([key, item]) => `${JSON.stringify(key)}:${canonicalContract(item)}`)
    .join(',')}}`;
}
