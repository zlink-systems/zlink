import { RoutingId as BindingRoutingId } from '@zlink-systems/zlink';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkLocationStore,
  type ZLinkStoreKey,
  type ZLinkStoreMutation,
  type ZLinkStoreReadResult,
  type ZLinkStoreScanCursor,
  type ZLinkStoreWriteRequest
} from '../../contracts';
import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import { macrotaskBoundary } from '../abort';
import { capacityCounterKey, capacityScopeForRidHex, spotTypeDimension } from './capacity-counters';
import { PROVIDER_STORAGE_NAMESPACE_PREFIX, storeKey } from './in-memory-provider-location-store';
import { readStoreCounter, validateStoreWriteRequest } from './provider-write-request';

const FORMAT_KEY = storeKey('zlink:v12:format');
export const GENERATION_FLOOR_KEY = storeKey('zlink:v12:generation-floor');
const CONVERTED = Buffer.from('converted');
export const AGGREGATE_PREFIX = PROVIDER_STORAGE_NAMESPACE_PREFIX + 'aggregate:';
const LEGACY_CAPACITY = PROVIDER_STORAGE_NAMESPACE_PREFIX + 'capacity:';
const LEGACY_OBJECT_COUNTER = storeKey(PROVIDER_STORAGE_NAMESPACE_PREFIX + 'object-counter');
const LEGACY_OWNER_COUNTER = storeKey(
  PROVIDER_STORAGE_NAMESPACE_PREFIX + 'authority-owner-counter'
);

type Put = Extract<ZLinkStoreMutation, { kind: 'put' }>;
type DescriptorRows = {
  prefix(mesh: string): string;
  routingIdHex(bytes: Uint8Array): string;
};

/** Owns the one-time startup conversion over the opaque Location Store SPI (§8.1). */
export class ZLinkLocationStoreFormat {
  private readonly descriptorRids = new Map<string, Map<string, Set<string>>>();

  constructor(
    private readonly provider: ZLinkLocationStore,
    private readonly descriptors: DescriptorRows,
    private readonly signal?: AbortSignal
  ) {}

  async convert(): Promise<bigint> {
    const completed = await this.readCompletedFloor();
    if (completed !== undefined) return completed;

    const raced = await this.checkAggregates();
    if (raced !== undefined) return raced;
    const floor = await this.ensureFloor();
    await this.moveCapacity();
    this.validateMarker(await this.putMissing(FORMAT_KEY, CONVERTED));
    return floor;
  }

  private async readCompletedFloor(): Promise<bigint | undefined> {
    const marker = await this.read(FORMAT_KEY);
    if (marker.kind === 'missing') return undefined;
    this.validateMarker(marker.value.bytes);
    const floor = await this.read(GENERATION_FLOOR_KEY);
    if (floor.kind === 'missing') {
      throw new ZLinkConfigurationException(
        'The converted Location Store has no generation floor.'
      );
    }
    return migrationGeneration(floor);
  }

  private validateMarker(bytes: Uint8Array): void {
    if (!CONVERTED.equals(bytes)) {
      throw new ZLinkConfigurationException('The Location Store format is unrecognized.');
    }
  }

  private async checkAggregates(): Promise<bigint | undefined> {
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const result = await this.scan(AGGREGATE_PREFIX, cursor);
      if (result.kind === 'expired') {
        cursor = undefined;
        await macrotaskBoundary();
        continue;
      }
      for (const { key, value } of result.value.items) {
        if (!isTerminalAggregate(key, value.bytes)) {
          // A v12 caller may start an aggregate after another startup publishes the marker.
          const completed = await this.readCompletedFloor();
          if (completed !== undefined) return completed;
          throw new ZLinkConfigurationException(
            `Unfinished v11 aggregate ${key.value} must be terminated in v11 before startup.`
          );
        }
      }
      cursor = result.value.nextCursor;
      if (cursor === undefined) return undefined;
    }
  }

  private async ensureFloor(): Promise<bigint> {
    const floor = await this.read(GENERATION_FLOOR_KEY);
    if (floor.kind === 'found') return migrationGeneration(floor);

    const [legacyObject, legacyOwner] = await Promise.all([
      this.read(LEGACY_OBJECT_COUNTER),
      this.read(LEGACY_OWNER_COUNTER)
    ]);
    const object = migrationGeneration(legacyObject);
    const owner = migrationGeneration(legacyOwner);
    const candidate = object > owner ? object : owner;
    const bytes = await this.putMissing(GENERATION_FLOOR_KEY, Buffer.from(candidate.toString()));
    return decodeGeneration(bytes);
  }

  private async putMissing(key: ZLinkStoreKey, bytes: Buffer): Promise<Uint8Array> {
    const request: ZLinkStoreWriteRequest = {
      conditions: [{ kind: 'missing', key }],
      mutations: [{ kind: 'put', key, bytes }]
    };
    for (;;) {
      const result = await this.write(request);
      if (result === 'applied') return bytes;
      const current = await this.read(key);
      if (current.kind === 'found') return current.value.bytes;
      if (result !== 'conflict') throw result;
      await macrotaskBoundary();
    }
  }

  private async moveCapacity(): Promise<void> {
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const result = await this.scan(LEGACY_CAPACITY, cursor);
      if (result.kind === 'expired') {
        cursor = undefined;
        await macrotaskBoundary();
        continue;
      }
      const { items, nextCursor } = result.value;
      // An empty fresh snapshot is the only completion condition.
      if (cursor === undefined && items.length === 0 && nextCursor === undefined) return;
      for (const { key } of items) await this.moveCapacityRow(key);
      cursor = nextCursor;
    }
  }

  private async moveCapacityRow(key: ZLinkStoreKey): Promise<void> {
    const { mesh, rawRid } = decodeLegacyKey(key);
    const rids = await this.readDescriptorRids(mesh);
    // Another startup can publish descriptors after every legacy row has moved.
    let current = await this.read(key);
    if (current.kind === 'missing') return;
    const rid = resolveRid(key, rawRid, rids);
    for (;;) {
      const bytes = current.value.bytes;
      const puts = capacityPuts(key, bytes, mesh, rid);
      const request: ZLinkStoreWriteRequest = {
        conditions: [
          { kind: 'value', key, expected: bytes },
          ...puts.map(({ key }) => ({ kind: 'missing' as const, key }))
        ],
        mutations: [...puts, { kind: 'delete', key }]
      };
      try {
        validateStoreWriteRequest(request);
      } catch (error) {
        if (!(error instanceof RangeError)) throw error;
        throw new ZLinkConfigurationException(
          `v11 capacity conversion exceeds a Location Store batch bound for ${key.value}. ${error.message}`
        );
      }
      const result = await this.write(request);
      if (result === 'applied') return;

      // Read the old row after the destinations so a concurrent atomic move cannot
      // look like a live row with conflicting counters.
      let present = 0;
      for (const put of puts) {
        if ((await this.read(put.key)).kind === 'found') present++;
      }
      current = await this.read(key);
      if (current.kind === 'missing') {
        if (present !== puts.length) {
          // A move may have won between the two observations.
          for (const put of puts) {
            if ((await this.read(put.key)).kind === 'missing') {
              throw new ZLinkConfigurationException(
                `Converted v11 capacity counter ${put.key.value} is missing.`
              );
            }
          }
        }
        return;
      }
      if (present !== 0 && Buffer.from(bytes).equals(current.value.bytes)) {
        throw new ZLinkConfigurationException(
          `A destination counter for v11 capacity row ${key.value} already exists.`
        );
      }
      if (result !== 'conflict') throw result;
      await macrotaskBoundary();
    }
  }

  private async readDescriptorRids(mesh: string): Promise<Map<string, Set<string>>> {
    const cached = this.descriptorRids.get(mesh);
    if (cached !== undefined) return cached;
    const rids = new Map<string, Set<string>>();
    const prefix = this.descriptors.prefix(mesh);
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const result = await this.scan(prefix, cursor);
      if (result.kind === 'expired') {
        rids.clear();
        cursor = undefined;
        await macrotaskBoundary();
        continue;
      }
      for (const { value } of result.value.items) {
        const rid = BindingRoutingId.fromHex(this.descriptors.routingIdHex(value.bytes));
        const hex = rid.toHex();
        for (const alias of [hex, Buffer.from(rid.toString()).toString('hex')]) {
          const candidates = rids.get(alias) ?? new Set<string>();
          candidates.add(hex);
          rids.set(alias, candidates);
        }
      }
      cursor = result.value.nextCursor;
      if (cursor === undefined) {
        this.descriptorRids.set(mesh, rids);
        return rids;
      }
    }
  }

  private read(key: ZLinkStoreKey) {
    this.signal?.throwIfAborted();
    return this.provider.read(key, this.signal);
  }

  private scan(prefix: string, cursor: ZLinkStoreScanCursor | undefined) {
    this.signal?.throwIfAborted();
    return this.provider.scan({ prefix, cursor, limit: ZLINK_PROVIDER_MAX_PAGE_SIZE }, this.signal);
  }

  private async write(
    request: ZLinkStoreWriteRequest
  ): Promise<'applied' | 'conflict' | ZLinkFrameworkException> {
    this.signal?.throwIfAborted();
    try {
      const result = await this.provider.write(request, this.signal);
      if (result.kind !== 'corrupt') return result.kind;
      throw new ZLinkConfigurationException('The Location Store rejected a conversion batch.');
    } catch (error) {
      this.signal?.throwIfAborted();
      if (
        error instanceof ZLinkFrameworkException &&
        error.kind === ZLinkFrameworkErrorKind.Unavailable
      ) {
        return error;
      }
      throw error;
    }
  }
}

function migrationGeneration(read: ZLinkStoreReadResult): bigint {
  return read.kind === 'missing' ? 1n : decodeGeneration(read.value.bytes);
}

function decodeGeneration(bytes: Uint8Array): bigint {
  const value = readStoreCounter(bytes);
  if (value === undefined || value === 0n) {
    throw new ZLinkConfigurationException('Invalid Location Store generation floor input.');
  }
  return value;
}

function decodeLegacyKey(key: ZLinkStoreKey): { mesh: string; rawRid: Buffer } {
  const suffix = key.value.slice(LEGACY_CAPACITY.length);
  const separator = suffix.indexOf(':');
  try {
    const mesh = decodeURIComponent(suffix.slice(0, separator));
    const encoded = suffix.slice(separator + 1);
    if (separator <= 0 || mesh.includes('\0') || encoded.length === 0 || encoded.includes(':')) {
      throw new TypeError('Invalid v11 capacity key segments.');
    }
    const parts = encoded.match(/%[0-9a-f]{2}|[^%]+|%/gi)!;
    const rawRid = Buffer.concat(
      parts.map((part) => {
        if (!part.startsWith('%')) return Buffer.from(part);
        if (part.length !== 3) throw new TypeError('Invalid percent escape in RoutingId.');
        return Buffer.from(part.slice(1), 'hex');
      })
    );
    return { mesh, rawRid };
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof URIError)) throw error;
    throw new ZLinkConfigurationException(`Malformed v11 capacity key ${key.value}.`);
  }
}

function resolveRid(key: ZLinkStoreKey, raw: Buffer, aliases: Map<string, Set<string>>): string {
  const literal = raw.toString('hex');
  const candidates = aliases.get(literal);
  if (candidates !== undefined) {
    if (candidates.size > 1) {
      throw new ZLinkConfigurationException(
        `Ambiguous RoutingId in v11 capacity key ${key.value}.`
      );
    }
    return candidates.values().next().value!;
  }
  try {
    const display = raw.toString('utf8');
    return raw.length > ZLINK_MAX_ROUTING_ID_BYTES && display.startsWith('hex:')
      ? BindingRoutingId.fromHex(display.slice(4)).toHex()
      : BindingRoutingId.from(raw).toHex();
  } catch (error) {
    if (!(error instanceof RangeError || error instanceof TypeError)) throw error;
    throw new ZLinkConfigurationException(`Invalid RoutingId in v11 capacity key ${key.value}.`);
  }
}

function isTerminalAggregate(key: ZLinkStoreKey, bytes: Uint8Array): boolean {
  // Java writes a state byte followed by the binary UUID and signed-long generation.
  // A committed header starts with state 2, never a valid JSON leading byte.
  if (bytes[0] === 2 && bytes.byteLength >= 25) {
    const header = Buffer.from(bytes.buffer, bytes.byteOffset, 25);
    const id = header.subarray(1, 17).toString('hex');
    const uuid = [
      id.slice(0, 8),
      id.slice(8, 12),
      id.slice(12, 16),
      id.slice(16, 20),
      id.slice(20)
    ].join('-');
    return key.value === `${AGGREGATE_PREFIX}${uuid}:${header.readBigInt64BE(17)}`;
  }
  try {
    const record = jsonObject(JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(bytes)));
    const phase = record.state ?? record.status;
    return phase === 'committed' || phase === 'aborted' || phase === 2 || phase === 3;
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof SyntaxError)) throw error;
    return false;
  }
}

function jsonObject(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new TypeError('Expected a JSON object.');
  }
  return value as Record<string, unknown>;
}

// Node 20 JSON.parse rounds wide integers. Preserve number tokens while rejecting JSON strings as counts.
const JSON_TOKEN = /"(?:[^"\\]|\\[\s\S])*"(\s*:)?|(-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)/g;

function capacityPuts(key: ZLinkStoreKey, bytes: Uint8Array, mesh: string, ridHex: string): Put[] {
  const counters: Put[] = [];
  const scope = capacityScopeForRidHex(mesh, ridHex);
  try {
    // Validate the original JSON grammar before rewriting any numeric tokens.
    const json = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
    JSON.parse(json);
    const root = jsonObject(
      JSON.parse(
        json.replace(JSON_TOKEN, (token, property, number) => {
          if (typeof number === 'string') return JSON.stringify(number);
          return property === undefined ? 'null' : token;
        })
      )
    );

    const nested = Object.hasOwn(root, 'active') || Object.hasOwn(root, 'pending');
    const typeDelimiter = nested ? '\0' : ':';
    // C++ v11 flat rows store the placement_object_kind_t ordinal as decimal.
    const userSpotKind = nested ? 'user_spot' : '2';
    const instanceSpotKind = nested ? 'instance_spot' : '3';
    for (const state of ['active', 'pending'] as const) {
      const suffix = state === 'active' ? 'Active' : 'Pending';
      const usage = nested ? jsonObject(root[state]) : root;
      add('actors', state, usage[nested ? 'actors' : 'actors' + suffix]);
      add('spots', state, usage[nested ? 'spots' : 'spots' + suffix]);
      for (const [type, value] of Object.entries(jsonObject(usage.spotTypes))) {
        const separator = type.indexOf(typeDelimiter);
        const legacyKind = type.slice(0, separator);
        const stableType = type.slice(separator + 1);
        if (
          separator < 0 ||
          (legacyKind !== userSpotKind && legacyKind !== instanceSpotKind) ||
          stableType.length === 0 ||
          stableType.includes('\0')
        )
          throw new TypeError('Invalid capacity spot type.');
        const objectKind = legacyKind === userSpotKind ? 'user_spot' : 'instance_spot';
        add(
          spotTypeDimension({ objectKind, stableType }),
          state,
          nested ? value : jsonObject(value)[state]
        );
      }
    }
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof SyntaxError)) throw error;
    throw new ZLinkConfigurationException(
      `Invalid v11 capacity record ${key.value}. ${error.message}`
    );
  }
  return counters;

  function add(dimension: string, state: 'active' | 'pending', value: unknown) {
    const decimal = typeof value === 'string' ? Buffer.from(value) : undefined;
    if (decimal === undefined || readStoreCounter(decimal) === undefined) {
      throw new TypeError('Invalid capacity count.');
    }
    if (value !== '0') {
      counters.push({
        kind: 'put',
        key: capacityCounterKey(scope, dimension, state),
        bytes: decimal
      });
    }
  }
}
