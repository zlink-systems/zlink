import { RoutingId } from '@zlink-systems/zlink';
import type { ZLinkStoreKey } from '../../contracts';
import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';
import { PROVIDER_STORAGE_NAMESPACE_PREFIX } from './in-memory-provider-location-store';
import type { ZLinkCapacityVector } from './internal-location-contracts';

export const AGGREGATE_PREFIX = PROVIDER_STORAGE_NAMESPACE_PREFIX + 'aggregate:';

export interface LegacyAggregate {
  readonly layout: 'node' | 'java' | 'dotnet' | 'cpp';
  readonly phase: 'staging' | 'prepared' | 'committed' | 'aborted' | 'committing';
  readonly id: string;
  readonly generation: bigint;
  readonly ownerId: string;
  readonly leaseGeneration: bigint;
  readonly mesh: string;
  readonly rid: string;
  readonly ridHex?: string;
  readonly lifecycleGeneration: bigint;
  readonly capacity: ZLinkCapacityVector;
  readonly commitPageCount?: number;
}

// Match strings separately so a decimal inside a string is never rewritten.
export const JSON_TOKEN = /"(?:[^"\\]|\\[\s\S])*"(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)/g;

export function jsonObject(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError('Expected a JSON object.');
  }
  return value as Record<string, unknown>;
}

export function readLegacyJson(bytes: Uint8Array): Record<string, unknown> {
  const json = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  JSON.parse(json);
  return jsonObject(
    JSON.parse(
      json.replace(JSON_TOKEN, (token, _property, number) =>
        typeof number === 'string' ? JSON.stringify(number) : token
      )
    )
  );
}

export function decodeLegacyAggregate(key: ZLinkStoreKey, bytes: Uint8Array): LegacyAggregate {
  try {
    const tag = bytes.at(0);
    if (tag !== undefined && tag <= 2) return decodeJavaAggregate(key, bytes);
    const record = readLegacyJson(bytes);
    const suffix = key.value.slice(AGGREGATE_PREFIX.length);
    if (record.header === undefined && record.state === undefined) {
      const id = text(record.aggregateId);
      if (suffix !== id || !/^[0-9a-f]{32}$/.test(id)) throw new TypeError('Invalid C++ root key.');
      return {
        layout: 'cpp',
        phase: phase(record.status),
        id,
        generation: generation(record.aggregateGeneration),
        ...owner(record.targetOwner),
        mesh: text(record.targetMeshName),
        rid: text(record.targetNodeRid),
        lifecycleGeneration: generation(record.targetLifecycleGeneration),
        capacity: capacity(record.capacityBundle),
        commitPageCount:
          record.commitPageCount === undefined ? undefined : count(record.commitPageCount)
      };
    }

    const node = record.header === undefined;
    const header = node ? record : jsonObject(record.header);
    const separator = suffix.lastIndexOf(':');
    if (separator <= 0) throw new TypeError('Invalid aggregate root key.');
    const id = decodeURIComponent(suffix.slice(0, separator));
    const aggregateGeneration = generation(suffix.slice(separator + 1));
    if (
      !node &&
      (text(header.aggregateId).replaceAll('-', '').toLowerCase() !== id ||
        generation(header.aggregateGeneration) !== aggregateGeneration)
    ) {
      throw new TypeError('The aggregate header does not match its root key.');
    }
    const target = jsonObject(header.targetDescriptor);
    const ridHex = node ? nodeRoutingIdHex(target.rid) : text(target.routingIdHex);
    return {
      layout: node ? 'node' : 'dotnet',
      phase: phase(node ? record.state : record.status),
      id,
      generation: aggregateGeneration,
      ...owner(header.targetOwner),
      mesh: text(target.meshName),
      rid: legacyRoutingIdDisplay(target.rid, ridHex),
      ridHex,
      lifecycleGeneration: generation(header.targetDescriptorLifecycleGeneration),
      capacity: capacity(header.capacity)
    };
  } catch (error) {
    if (!(
      error instanceof TypeError ||
      error instanceof SyntaxError ||
      error instanceof RangeError
    )) {
      throw error;
    }
    throw new ZLinkConfigurationException(
      `Cannot settle v11 aggregate ${key.value}. ${error.message}`
    );
  }
}

function decodeJavaAggregate(key: ZLinkStoreKey, bytes: Uint8Array): LegacyAggregate {
  const buffer = Buffer.from(bytes);
  let offset = 0;
  const take = (length: number) => {
    if (length < 0 || offset + length > buffer.length) {
      throw new RangeError('Truncated Java aggregate.');
    }
    const value = buffer.subarray(offset, offset + length);
    offset += length;
    return value;
  };
  const readInt = () => take(4).readInt32BE();
  const readGeneration = () => generation(take(8).readBigInt64BE().toString());
  const readUtf = () => javaModifiedUtf(take(take(2).readUInt16BE()));
  const state = take(1)[0]!;
  const hex = take(16).toString('hex');
  const id = `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
  const aggregateGeneration = readGeneration();
  if (key.value !== `${AGGREGATE_PREFIX}${id}:${aggregateGeneration}`) {
    throw new TypeError('The Java aggregate header does not match its root key.');
  }
  const mesh = text(readUtf());
  const ridHex = text(readUtf());
  const lifecycleGeneration = readGeneration();
  const ownerId = text(readUtf());
  const leaseGeneration = readGeneration();
  if (readInt() !== 32) throw new TypeError('Invalid Java inventory digest length.');
  take(32);
  const actors = count(readInt());
  const spots = count(readInt());
  const hasSpotType = take(1)[0]!;
  if (hasSpotType > 1) throw new TypeError('Invalid Java spot type flag.');
  const spotType =
    hasSpotType === 0
      ? undefined
      : {
          objectKind: spotKind(readInt() + 1),
          stableType: text(readUtf()),
          count: count(readInt())
        };
  if (readInt() < 1 || readInt() !== 32) throw new TypeError('Invalid Java participant metadata.');
  take(32);
  if (offset !== buffer.length) throw new TypeError('Unexpected Java aggregate bytes.');
  return {
    layout: 'java',
    phase: phase(String(state)),
    id,
    generation: aggregateGeneration,
    ownerId,
    leaseGeneration,
    mesh,
    rid: legacyRoutingIdDisplay(undefined, ridHex),
    ridHex,
    lifecycleGeneration,
    capacity: { actors, spots, spotType }
  };
}

// DataOutputStream.writeUTF emits UTF-16 code units, including surrogate pairs.
function javaModifiedUtf(bytes: Buffer): string {
  let result = '';
  for (let offset = 0; offset < bytes.length;) {
    const first = bytes[offset++]!;
    if (first > 0 && first < 0x80) {
      result += String.fromCharCode(first);
      continue;
    }
    const second = bytes.at(offset++);
    if (second === undefined || (second & 0xc0) !== 0x80) throw new TypeError('Invalid Java UTF.');
    if ((first & 0xe0) === 0xc0) {
      result += String.fromCharCode(((first & 0x1f) << 6) | (second & 0x3f));
      continue;
    }
    const third = bytes.at(offset++);
    if ((first & 0xf0) !== 0xe0 || third === undefined || (third & 0xc0) !== 0x80) {
      throw new TypeError('Invalid Java UTF.');
    }
    result += String.fromCharCode(((first & 0x0f) << 12) | ((second & 0x3f) << 6) | (third & 0x3f));
  }
  return result;
}

function phase(value: unknown): LegacyAggregate['phase'] {
  switch (value) {
    case '0':
    case 'preparing':
    case 'staging':
      return 'staging';
    case '1':
    case 'prepared':
      return 'prepared';
    case '2':
    case 'committed':
      return 'committed';
    case '3':
    case 'aborted':
      return 'aborted';
    case 'committing':
      return 'committing';
    default:
      throw new TypeError('Invalid aggregate phase.');
  }
}

function owner(value: unknown) {
  const record = jsonObject(value);
  return { ownerId: text(record.ownerId), leaseGeneration: generation(record.leaseGeneration) };
}

export function generation(value: unknown): bigint {
  const decimal = typeof value === 'object' ? jsonObject(value).$bigint : value;
  if (typeof decimal !== 'string' || !/^[1-9]\d{0,19}$/.test(decimal)) {
    throw new TypeError('Invalid aggregate generation.');
  }
  const result = BigInt(decimal);
  if (result > (1n << 64n) - 1n) throw new RangeError('Invalid aggregate generation.');
  return result;
}

function count(value: unknown): number {
  const decimal = typeof value === 'number' ? String(value) : value;
  if (typeof decimal !== 'string' || !/^(?:0|[1-9]\d*)$/.test(decimal)) {
    throw new TypeError('Invalid aggregate capacity count.');
  }
  const number = Number(decimal);
  if (!Number.isSafeInteger(number)) throw new RangeError('Invalid aggregate capacity count.');
  return number;
}

function capacity(value: unknown): ZLinkCapacityVector {
  const record = jsonObject(value);
  const type = record.spotType == null ? undefined : jsonObject(record.spotType);
  return {
    actors: count(record.actors),
    spots: count(record.spots),
    spotType:
      type === undefined
        ? undefined
        : {
            objectKind: spotKind(type.objectKind),
            stableType: text(type.stableType),
            count: count(type.count)
          }
  };
}

function spotKind(value: unknown): 'user_spot' | 'instance_spot' {
  switch (String(value)) {
    case '2':
    case 'userSpot':
    case 'user_spot':
      return 'user_spot';
    case '3':
    case 'instanceSpot':
    case 'instance_spot':
      return 'instance_spot';
    default:
      throw new TypeError('Invalid aggregate spot type.');
  }
}

function text(value: unknown): string {
  if (typeof value !== 'string' || value.length === 0 || value.includes('\0')) {
    throw new TypeError('Invalid aggregate text field.');
  }
  return value;
}

function nodeRoutingIdHex(value: unknown): string | undefined {
  if (typeof value === 'string') return undefined;
  const rid = jsonObject(value);
  if (typeof rid.hex === 'string') return rid.hex;
  const bytes = jsonObject(rid._bytes);
  if (bytes.type !== 'Buffer' || !Array.isArray(bytes.data)) {
    throw new TypeError('Invalid Node aggregate RoutingId.');
  }
  const octets = bytes.data.map(count);
  if (octets.some((octet) => octet > 255)) throw new TypeError('Invalid Node RoutingId byte.');
  return Buffer.from(octets).toString('hex');
}

function legacyRoutingIdDisplay(value: unknown, hex: string | undefined): string {
  if (hex === undefined) return text(value);
  if (!/^(?:[0-9a-fA-F]{2}){1,255}$/.test(hex)) {
    throw new TypeError('Invalid v11 aggregate RoutingId.');
  }
  const routingId = RoutingId.fromHex(hex);
  if (value !== null && typeof value === 'object' && 'text' in value) {
    return text(value.text);
  }
  return routingId.toString();
}
