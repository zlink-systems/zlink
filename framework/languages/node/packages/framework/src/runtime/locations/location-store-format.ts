import { createHash } from 'node:crypto';
import { RoutingId as BindingRoutingId } from '@zlink-systems/zlink';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkLocationStore,
  type ZLinkStoreCondition,
  type ZLinkStoreKey,
  type ZLinkStoreScanItem,
  type ZLinkStoreMutation,
  type ZLinkStoreReadResult,
  type ZLinkStoreScanCursor,
  type ZLinkStoreWriteRequest
} from '../../contracts';
import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import { ZLinkConfigurationException } from '../../contracts/Configuration/ConfigurationException';
import { ZLINK_PROVIDER_MAX_PAGE_SIZE } from '../../contracts/Locations/Stores';
import { macrotaskBoundary } from '../abort';
import {
  capacityCounterKey,
  capacityDimensions,
  capacityScopeForRidHex,
  spotTypeDimension
} from './capacity-counters';
import { PROVIDER_STORAGE_NAMESPACE_PREFIX, storeKey } from './in-memory-provider-location-store';
import { readStoreCounter, validateStoreWriteRequest } from './provider-write-request';
import {
  AGGREGATE_PREFIX,
  decodeLegacyAggregate,
  generation,
  jsonObject,
  JSON_TOKEN,
  readLegacyJson,
  type LegacyAggregate
} from './legacy-aggregate-record';

const FORMAT_KEY = storeKey('zlink:v12:format');
export const GENERATION_FLOOR_KEY = storeKey('zlink:v12:generation-floor');
const CONVERTED = Buffer.from('converted');
const AGGREGATE_CHILD_PREFIX = PROVIDER_STORAGE_NAMESPACE_PREFIX + 'aggregate-';
const LEGACY_CAPACITY = PROVIDER_STORAGE_NAMESPACE_PREFIX + 'capacity:';
const LEGACY_OBJECT_COUNTER = storeKey(PROVIDER_STORAGE_NAMESPACE_PREFIX + 'object-counter');
const LEGACY_OWNER_COUNTER = storeKey(
  PROVIDER_STORAGE_NAMESPACE_PREFIX + 'authority-owner-counter'
);

type Put = Extract<ZLinkStoreMutation, { kind: 'put' }>;
type ConversionRecords = {
  prefix(mesh: string): string;
  routingIdHex(bytes: Uint8Array): string;
  ownerLeaseIsLive(aggregate: LegacyAggregate): Promise<boolean>;
  settleParticipants(root: ZLinkStoreScanItem, aggregate: LegacyAggregate): Promise<void>;
  authorityKey(logicalKey: string): ZLinkStoreKey;
};

/** Owns the one-time startup conversion over the opaque Location Store SPI (§8.1). */
export class ZLinkLocationStoreFormat {
  private readonly descriptorRids = new Map<string, Map<string, Set<string>>>();

  constructor(
    private readonly provider: ZLinkLocationStore,
    private readonly records: ConversionRecords,
    private readonly signal?: AbortSignal
  ) {}

  async convert(): Promise<bigint> {
    const completed = await this.readCompletedFloor();
    if (completed !== undefined) return completed;

    const raced = await this.settleAggregates();
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

  private async settleAggregates(): Promise<bigint | undefined> {
    const rows = new Map<string, ZLinkStoreScanItem>();
    for await (const items of this.scanPages(AGGREGATE_PREFIX)) {
      for (const item of items) {
        const suffix = item.key.value.slice(AGGREGATE_PREFIX.length);
        if (!/^.+:[1-9]\d*:(?:participant|inventory):/.test(suffix)) {
          rows.set(item.key.value, item);
        }
      }
    }
    if (rows.size === 0) return undefined;

    // Another startup may have published the marker and begun ordinary v12 relocation.
    const completed = await this.readCompletedFloor();
    if (completed !== undefined) return completed;
    const aggregates = [...rows.values()].map((root) => ({
      root,
      aggregate: decodeLegacyAggregate(root.key, root.value.bytes)
    }));
    for (const { root, aggregate } of aggregates) {
      if (await this.records.ownerLeaseIsLive(aggregate)) {
        const completed = await this.readCompletedFloor();
        if (completed !== undefined) return completed;
        throw new ZLinkConfigurationException(
          `Cannot settle v11 aggregate ${root.key.value} with a live target owner lease.`
        );
      }
    }

    for (const { root, aggregate } of aggregates) {
      const terminal =
        aggregate.phase === 'committing'
          ? await this.abortCppCommit(root, aggregate)
          : { root, aggregate };
      if (terminal !== undefined) await this.settleAggregate(terminal.root, terminal.aggregate);
    }
    return undefined;
  }

  private async abortCppCommit(root: ZLinkStoreScanItem, aggregate: LegacyAggregate) {
    const pageCount = aggregate.commitPageCount;
    if (pageCount === undefined || pageCount === 0) throw this.invalidCppCommit(root);
    const rootCondition = { kind: 'version' as const, key: root.key, expected: root.value.version };
    // Change only the status token so wide counters and generations keep their exact bytes.
    const json = Buffer.from(root.value.bytes).toString('utf8');
    const aborted = Buffer.from(json.replace(/("status"\s*:\s*)"committing"/, '$1"aborted"'));
    const terminal = (current: ZLinkStoreReadResult) => {
      if (current.kind === 'missing') return;
      if (!aborted.equals(current.value.bytes)) throw this.invalidCppCommit(root);
      return {
        root: { key: root.key, value: current.value },
        aggregate: { ...aggregate, phase: 'aborted' as const }
      };
    };

    for (let index = 0; index < pageCount; index++) {
      const page = await this.read(
        storeKey(`${AGGREGATE_CHILD_PREFIX}commit:${aggregate.id}:${index}`)
      );
      const current = await this.read(root.key);
      if (current.kind === 'missing' || current.value.version.value !== root.value.version.value) {
        return terminal(current);
      }
      if (page.kind === 'missing') throw this.invalidCppCommit(root);
      const conditions: ZLinkStoreCondition[] = [rootCondition];
      const mutations: ZLinkStoreMutation[] = [];
      for (const [entryIndex, entry] of cppCommitEntries(page.value.bytes, index).entries()) {
        const authorityKey = this.records.authorityKey(entry.authorityKey);
        const lockKey = storeKey(
          AGGREGATE_CHILD_PREFIX +
            'lock:' +
            createHash('sha256').update(entry.authorityKey).digest('hex')
        );
        const lock = await this.read(lockKey);
        const authority = await this.read(authorityKey);
        if (authority.kind === 'missing') throw this.invalidCppCommit(root);
        const actual = Buffer.from(authority.value.bytes);
        if (lock.kind === 'missing') {
          if (actual.equals(entry.before)) continue;
          throw this.invalidCppCommit(root);
        }
        const fence = readLegacyJson(lock.value.bytes);
        if (
          fence.aggregateId !== aggregate.id ||
          generation(fence.aggregateGeneration) !== aggregate.generation ||
          fence.status !== 'committing' ||
          fence.authorityKey !== entry.authorityKey ||
          fence.pageIndex !== String(index) ||
          fence.entryIndex !== String(entryIndex) ||
          (!actual.equals(entry.before) && !actual.equals(entry.after))
        ) {
          throw this.invalidCppCommit(root);
        }
        conditions.push(
          { kind: 'version', key: lockKey, expected: lock.value.version },
          { kind: 'version', key: authorityKey, expected: authority.value.version }
        );
        mutations.push({ kind: 'delete', key: lockKey });
        if (!actual.equals(entry.before))
          mutations.push({ kind: 'put', key: authorityKey, bytes: entry.before });
      }
      if (mutations.length !== 0) await this.writeAggregateBatch(root, { conditions, mutations });
    }

    const release = await this.legacyPendingRelease(root, aggregate);
    if (release !== undefined) {
      // Keep commit pages until rollback and pending release are durably recorded.
      await this.writeAggregateBatch(root, {
        conditions: [rootCondition, ...release.conditions],
        mutations: [...release.mutations, { kind: 'put', key: root.key, bytes: aborted }]
      });
    }
    return terminal(await this.read(root.key));
  }

  private invalidCppCommit(root: ZLinkStoreScanItem): ZLinkConfigurationException {
    return new ZLinkConfigurationException(`Cannot roll back v11 C++ aggregate ${root.key.value}.`);
  }

  private async settleAggregate(
    root: ZLinkStoreScanItem,
    aggregate: LegacyAggregate
  ): Promise<void> {
    const rootCondition = { kind: 'version' as const, key: root.key, expected: root.value.version };
    if ((await this.read(root.key)).kind === 'missing') return;
    const release = await this.legacyPendingRelease(root, aggregate);
    if (release === undefined) return;

    await this.records.settleParticipants(root, aggregate);
    const suffix = root.key.value.slice(AGGREGATE_PREFIX.length);
    const prefix = AGGREGATE_CHILD_PREFIX;
    if (aggregate.layout === 'cpp') {
      for await (const items of this.scanPages(prefix + 'lock:')) {
        const locks = items.filter(({ value }) => {
          const lock = readLegacyJson(value.bytes);
          return (
            lock.aggregateId === aggregate.id &&
            generation(lock.aggregateGeneration) === aggregate.generation
          );
        });
        await this.deleteAggregateChildren(root, locks);
      }
      for (const kind of ['inventory', 'inventory-index', 'commit']) {
        await this.deleteAggregatePrefix(root, `${prefix}${kind}:${suffix}:`);
      }
    } else if (aggregate.layout === 'dotnet') {
      for (const kind of ['participant', 'inventory']) {
        await this.deleteAggregatePrefix(root, `${root.key.value}:${kind}:`);
      }
    } else {
      await this.deleteAggregatePrefix(root, `${prefix}inventory:${suffix}:`);
      if (aggregate.layout === 'node') {
        await this.deleteAggregatePrefix(root, `${prefix}participant:${suffix}:`);
      }
    }
    await this.writeAggregateBatch(root, {
      conditions: [rootCondition, ...release.conditions],
      mutations: [...release.mutations, { kind: 'delete', key: root.key }]
    });
  }

  private async legacyPendingRelease(root: ZLinkStoreScanItem, aggregate: LegacyAggregate) {
    const dimensions = capacityDimensions(aggregate.capacity);
    const changes = {
      conditions: [] as ZLinkStoreWriteRequest['conditions'][number][],
      mutations: [] as ZLinkStoreMutation[]
    };
    if (
      (aggregate.phase !== 'prepared' && aggregate.phase !== 'committing') ||
      dimensions.length === 0
    )
      return changes;

    const { mesh, rid, ridHex } = aggregate;
    const rawRid = ridHex === undefined ? undefined : Buffer.from(ridHex, 'hex');
    const encodedRid = encodeURIComponent(rid);
    const key = storeKey(LEGACY_CAPACITY + encodeURIComponent(mesh) + ':' + encodedRid);
    let row = await this.read(key);
    let capacityKey = key;
    if (row.kind === 'missing' && rawRid !== undefined) {
      for await (const items of this.scanPages(LEGACY_CAPACITY + encodeURIComponent(mesh) + ':')) {
        const matches = items.filter((item) => decodeLegacyKey(item.key).rawRid.equals(rawRid));
        if (matches.length === 0) continue;
        if (row.kind === 'found' || matches.length > 1) {
          throw new ZLinkConfigurationException(
            `Ambiguous v11 capacity for aggregate ${root.key.value}.`
          );
        }
        capacityKey = matches[0]!.key;
        row = { kind: 'found', value: matches[0]!.value };
      }
    }
    const current = await this.read(root.key);
    if (
      current.kind === 'missing' ||
      (aggregate.phase === 'committing' && current.value.version.value !== root.value.version.value)
    )
      return undefined;
    if (row.kind === 'missing') {
      throw new ZLinkConfigurationException(
        `Missing v11 capacity for aggregate ${root.key.value}.`
      );
    }
    const bytes = releasePending(capacityKey, row.value.bytes, dimensions);
    changes.conditions.push({ kind: 'version', key: capacityKey, expected: row.value.version });
    changes.mutations.push({ kind: 'put', key: capacityKey, bytes });
    return changes;
  }

  private async deleteAggregatePrefix(root: ZLinkStoreScanItem, prefix: string): Promise<void> {
    for await (const items of this.scanPages(prefix)) {
      await this.deleteAggregateChildren(root, items);
    }
  }

  private async deleteAggregateChildren(
    root: ZLinkStoreScanItem,
    items: readonly ZLinkStoreScanItem[]
  ): Promise<void> {
    if (items.length === 0) return;
    const request: ZLinkStoreWriteRequest = {
      conditions: [
        { kind: 'version', key: root.key, expected: root.value.version },
        ...items.map(({ key, value }) => ({
          kind: 'version' as const,
          key,
          expected: value.version
        }))
      ],
      mutations: items.map(({ key }) => ({ kind: 'delete', key }))
    };
    await this.writeAggregateBatch(root, request);
  }

  private async writeAggregateBatch(
    root: ZLinkStoreScanItem,
    request: ZLinkStoreWriteRequest
  ): Promise<void> {
    const result = await this.write(request);
    if (result === 'applied' || (await this.read(root.key)).kind === 'missing') return;
    // A concurrent startup or a lost reply can leave the requested values installed.
    const applied = await Promise.all(
      request.mutations.map(async (mutation) => {
        const current = await this.read(mutation.key);
        return mutation.kind === 'delete'
          ? current.kind === 'missing'
          : mutation.kind === 'put' &&
              current.kind === 'found' &&
              Buffer.from(mutation.bytes).equals(current.value.bytes);
      })
    );
    if (applied.every(Boolean) || (await this.read(root.key)).kind === 'missing') return;
    if (result !== 'conflict') throw result;
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.Unavailable,
      `The v11 aggregate conversion changed concurrently at ${root.key.value}.`
    );
  }

  private async *scanPages(prefix: string) {
    let cursor: ZLinkStoreScanCursor | undefined;
    for (;;) {
      const result = await this.scan(prefix, cursor);
      if (result.kind === 'expired') {
        // The conversion already uses the Store's expired cursor handling.
        cursor = undefined;
        await macrotaskBoundary();
        continue;
      }
      yield result.value.items;
      if (result.value.nextCursor === undefined) return;
      cursor = result.value.nextCursor;
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
    const prefix = this.records.prefix(mesh);
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
        const rid = BindingRoutingId.fromHex(this.records.routingIdHex(value.bytes));
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

function capacityPuts(key: ZLinkStoreKey, bytes: Uint8Array, mesh: string, ridHex: string): Put[] {
  const { fields } = decodeCapacityFields(key, bytes);
  const scope = capacityScopeForRidHex(mesh, ridHex);
  return fields.flatMap(({ dimension, state, count }) =>
    count === 0n
      ? []
      : [
          {
            kind: 'put' as const,
            key: capacityCounterKey(scope, dimension, state),
            bytes: Buffer.from(count.toString())
          }
        ]
  );
}

function releasePending(
  key: ZLinkStoreKey,
  bytes: Uint8Array,
  dimensions: ReturnType<typeof capacityDimensions>
): Buffer {
  const { root, fields } = decodeCapacityFields(key, bytes);
  for (const [dimension, count] of dimensions) {
    const field = fields.find(
      (field) => field.state === 'pending' && field.dimension === dimension
    );
    if (field === undefined || field.count < BigInt(count)) {
      throw new ZLinkConfigurationException(
        `Invalid pending v11 aggregate capacity in ${key.value}.`
      );
    }
    field.row[field.name] = (field.count - BigInt(count)).toString();
  }
  // The decoder preserves decimal tokens as strings. Emit numbers again without rounding.
  return Buffer.from(
    JSON.stringify(root).replace(JSON_TOKEN, (token, property) =>
      property === undefined && /^"\d+"$/.test(token) ? token.slice(1, -1) : token
    )
  );
}

function decodeCapacityFields(key: ZLinkStoreKey, bytes: Uint8Array) {
  const fields: {
    dimension: string;
    state: 'active' | 'pending';
    row: Record<string, unknown>;
    name: string;
    count: bigint;
  }[] = [];
  try {
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
    const userSpotKind = nested ? 'user_spot' : '2';
    const instanceSpotKind = nested ? 'instance_spot' : '3';
    for (const state of ['active', 'pending'] as const) {
      const suffix = state === 'active' ? 'Active' : 'Pending';
      const usage = nested ? jsonObject(root[state]) : root;
      add('actors', state, usage, nested ? 'actors' : 'actors' + suffix);
      add('spots', state, usage, nested ? 'spots' : 'spots' + suffix);
      const spotTypes = jsonObject(usage.spotTypes);
      for (const [type, value] of Object.entries(spotTypes)) {
        const separator = type.indexOf(typeDelimiter);
        const legacyKind = type.slice(0, separator);
        const stableType = type.slice(separator + 1);
        if (
          separator < 0 ||
          (legacyKind !== userSpotKind && legacyKind !== instanceSpotKind) ||
          stableType.length === 0 ||
          stableType.includes('\0')
        ) {
          throw new TypeError('Invalid capacity spot type.');
        }
        const objectKind = legacyKind === userSpotKind ? 'user_spot' : 'instance_spot';
        add(
          spotTypeDimension({ objectKind, stableType }),
          state,
          nested ? spotTypes : jsonObject(value),
          nested ? type : state
        );
      }
    }
    return { root, fields };
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof SyntaxError)) throw error;
    throw new ZLinkConfigurationException(
      `Invalid v11 capacity record ${key.value}. ${error.message}`
    );
  }

  function add(
    dimension: string,
    state: 'active' | 'pending',
    row: Record<string, unknown>,
    name: string
  ) {
    const value = row[name];
    const count = typeof value === 'string' ? readStoreCounter(Buffer.from(value)) : undefined;
    if (count === undefined) throw new TypeError('Invalid capacity count.');
    fields.push({ dimension, state, row, name, count });
  }
}

function cppCommitEntries(bytes: Uint8Array, index: number) {
  try {
    const page = readLegacyJson(bytes);
    if (
      page.version !== '1' ||
      page.pageIndex !== String(index) ||
      !Array.isArray(page.entries) ||
      page.entries.length === 0 ||
      page.entries.length > 512
    ) {
      throw new TypeError('Invalid C++ commit page header.');
    }
    return page.entries.map((value: unknown) => {
      const entry = jsonObject(value);
      if (
        typeof entry.authorityKey !== 'string' ||
        typeof entry.before !== 'string' ||
        !/^(?:[0-9a-f]{2})+$/.test(entry.before) ||
        typeof entry.after !== 'string' ||
        !/^(?:[0-9a-f]{2})+$/.test(entry.after)
      ) {
        throw new TypeError('Invalid C++ commit page entry.');
      }
      return {
        authorityKey: entry.authorityKey,
        before: Buffer.from(entry.before, 'hex'),
        after: Buffer.from(entry.after, 'hex')
      };
    });
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof SyntaxError)) throw error;
    throw new ZLinkConfigurationException(
      `Cannot decode v11 C++ commit page ${index}. ${error.message}`
    );
  }
}
