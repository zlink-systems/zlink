import { createHash, randomUUID } from 'node:crypto';
import type {
  ZLinkLocationStore,
  ZLinkStoreKey,
  ZLinkStoreReadResult,
  ZLinkStoreScanCursor,
  ZLinkStoreScanRequest,
  ZLinkStoreScanResult,
  ZLinkStoreVersion,
  ZLinkStoreWriteRequest,
  ZLinkStoreWriteResult
} from '@zlink-systems/framework';
import type { ZLinkRedisLocationOptions } from './redis-options';
import { RedisConnection } from './redis-connection';
import {
  OPAQUE_READ_SCRIPT,
  OPAQUE_SCAN_CONTINUE_SCRIPT,
  OPAQUE_SCAN_START_SCRIPT,
  OPAQUE_WRITE_SCRIPT
} from './opaque-redis-scripts';
import { asArray, asString, toNumber } from './redis-values';

const MAX_VALUE_BYTES = 1024 * 1024;
const MAX_WRITE_KEYS = 2_048;
const MAX_WRITE_BYTES = 4 * 1024 * 1024;

// {prefix}:{zlink-location-v3}:opaque:{sha256hex(preimage)} is the public
// contract (21-location-runtime.md#2.4, 22-location-store-redis.md#7). The
// braces are a Redis Cluster hash tag: every key this provider's scripts
// touch in one EVAL (the record row plus the private auxiliary keys below)
// must land on the same hash slot, matching the dotnet/java reference. Only
// the six auxiliary keys below (index/map/cleanup/sequence/snapshot*) are a
// private implementation detail of this provider's point-in-time scan.
const NAMESPACE = '{zlink-location-v3}:opaque';

/** Redis implementation of the opaque Location Store provider SPI. */
export class ZLinkRedisLocationStore implements ZLinkLocationStore {
  private readonly connection: RedisConnection;
  private readonly domain: string;

  constructor(options: ZLinkRedisLocationOptions) {
    this.connection = new RedisConnection(options);
    this.domain = `${options.keyPrefix}:${NAMESPACE}`;
  }

  async read(
    key: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<ZLinkStoreReadResult> {
    const logicalKey = requireKey(key);
    const result = asArray(await this.connection.eval(
      OPAQUE_READ_SCRIPT,
      [this.rowKey(logicalKey)],
      [],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (toNumber(result[0]) !== 1) return { kind: 'missing', storeNow };
    requireMatchingKey(asString(result[2]), logicalKey);
    return {
      kind: 'found',
      value: {
        bytes: rawBytes(result[3]),
        version: storeVersion(asString(result[4])),
        expiresAt: expiresAtOrUndefined(toNumber(result[5])),
        storeNow
      }
    };
  }

  async write(
    request: ZLinkStoreWriteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreWriteResult> {
    const encoded = encodeWrite(request);
    const result = asArray(await this.connection.eval(
      OPAQUE_WRITE_SCRIPT,
      [
        this.indexKey(),
        this.mapKey(),
        this.cleanupKey(),
        this.sequenceKey(),
        this.snapshotExpiryKey(),
        this.snapshotBoundaryKey(),
        ...encoded.keys.map(key => this.rowKey(key))
      ],
      [
        JSON.stringify(encoded.conditions),
        JSON.stringify(encoded.mutations),
        ...encoded.putBytes
      ],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    const outcome = asString(result[0]);
    if (outcome === 'conflict') return { kind: 'conflict', storeNow };
    if (outcome === 'backlog') {
      throw new Error('Redis Location Store version backlog is full.');
    }
    if (outcome !== 'applied') {
      throw new Error('Redis Location Store returned an unrecognized write outcome.');
    }
    const putVersions = [];
    for (let index = 2; index < result.length; index += 2) {
      putVersions.push({
        key: storeKey(asString(result[index])),
        version: storeVersion(asString(result[index + 1]))
      });
    }
    return { kind: 'applied', putVersions, storeNow };
  }

  async scan(
    request: ZLinkStoreScanRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreScanResult> {
    requireScanRequest(request);
    if (request.cursor === undefined) {
      const snapshotId = randomUUID();
      return await this.readScanPage(
        snapshotId,
        await this.connection.eval(
          OPAQUE_SCAN_START_SCRIPT,
          this.scanKeys(snapshotId),
          [request.prefix, String(request.limit), snapshotId],
          signal
        )
      );
    }
    const cursor = parseCursor(request.cursor);
    return await this.readScanPage(
      cursor.snapshotId,
      await this.connection.eval(
        OPAQUE_SCAN_CONTINUE_SCRIPT,
        this.scanKeys(cursor.snapshotId),
        [request.prefix, cursor.lastKey, String(request.limit), cursor.snapshotId],
        signal
      )
    );
  }

  async dispose(): Promise<void> {
    await this.connection.dispose();
  }

  private async readScanPage(
    snapshotId: string,
    raw: unknown
  ): Promise<ZLinkStoreScanResult> {
    const result = asArray(raw);
    const outcome = asString(result[0]);
    if (outcome === 'expired') return { kind: 'expired' };
    if (outcome === 'capacity') {
      throw new Error('Redis Location Store snapshot capacity is full.');
    }
    if (outcome !== 'page') {
      throw new Error('Redis Location Store returned an unrecognized scan outcome.');
    }
    const storeNow = fromUnixMs(toNumber(result[1]));
    const nextKey = asString(result[2]);
    const items = [];
    for (let index = 3; index < result.length; index += 4) {
      items.push({
        key: storeKey(asString(result[index])),
        value: {
          bytes: rawBytes(result[index + 1]),
          version: storeVersion(asString(result[index + 2])),
          expiresAt: expiresAtOrUndefined(toNumber(result[index + 3])),
          storeNow
        }
      });
    }
    return {
      kind: 'page',
      value: {
        items,
        nextCursor: nextKey.length === 0
          ? undefined
          : scanCursor(`${snapshotId}:${Buffer.from(nextKey, 'utf8').toString('hex')}`),
        storeNow
      }
    };
  }

  private scanKeys(snapshotId: string): readonly string[] {
    return [
      this.indexKey(),
      this.mapKey(),
      this.snapshotKey(snapshotId),
      this.cleanupKey(),
      this.sequenceKey(),
      this.snapshotExpiryKey(),
      this.snapshotBoundaryKey()
    ];
  }

  private indexKey(): string {
    return `${this.domain}:index`;
  }

  private mapKey(): string {
    return `${this.domain}:map`;
  }

  private cleanupKey(): string {
    return `${this.domain}:cleanup`;
  }

  private sequenceKey(): string {
    return `${this.domain}:sequence`;
  }

  private snapshotExpiryKey(): string {
    return `${this.domain}:snapshot-expiry`;
  }

  private snapshotBoundaryKey(): string {
    return `${this.domain}:snapshot-boundary`;
  }

  private snapshotKey(snapshotId: string): string {
    return `${this.domain}:scan:${snapshotId}`;
  }

  private rowKey(logicalKey: string): string {
    return `${this.domain}:${digest(logicalKey)}`;
  }
}

interface EncodedWrite {
  readonly keys: readonly string[];
  readonly conditions: readonly unknown[];
  readonly mutations: readonly unknown[];
  readonly putBytes: readonly Buffer[];
}

function encodeWrite(request: ZLinkStoreWriteRequest): EncodedWrite {
  const conditionKeys = request.conditions.map(condition => requireKey(condition.key));
  const mutationKeys = request.mutations.map(mutation => requireKey(mutation.key));
  if (
    new Set(conditionKeys).size !== conditionKeys.length
    || new Set(mutationKeys).size !== mutationKeys.length
  ) {
    throw new RangeError('Location Store condition and mutation keys must be unique.');
  }
  const keys = [...new Set([...conditionKeys, ...mutationKeys])];
  if (keys.length > MAX_WRITE_KEYS) {
    throw new RangeError('Location Store write exceeds 2,048 unique keys.');
  }
  // Row keys are appended after the six fixed auxiliary keys; the script
  // adds 6 to this 1-based index before indexing into KEYS.
  const keyIndex = new Map(keys.map((key, index) => [key, index + 1]));
  let encodedBytes = 0;
  const conditions = request.conditions.map(condition => {
    const key = requireKey(condition.key);
    encodedBytes += Buffer.byteLength(key, 'utf8');
    if (condition.kind === 'missing') return ['missing', keyIndex.get(key), key];
    const expected = requireVersion(condition.expected);
    encodedBytes += Buffer.byteLength(expected, 'utf8');
    return ['version', keyIndex.get(key), key, expected];
  });
  const putBytes: Buffer[] = [];
  const mutations = request.mutations.map(mutation => {
    const key = requireKey(mutation.key);
    encodedBytes += Buffer.byteLength(key, 'utf8');
    if (mutation.kind === 'delete') return ['delete', keyIndex.get(key), key];
    requireValue(mutation.bytes, mutation.retentionMs);
    encodedBytes += mutation.bytes.byteLength;
    putBytes.push(Buffer.from(mutation.bytes));
    return [
      'put',
      keyIndex.get(key),
      key,
      mutation.retentionMs ?? false
    ];
  });
  if (encodedBytes > MAX_WRITE_BYTES) {
    throw new RangeError('Location Store write exceeds 4 MiB encoded input.');
  }
  return { keys, conditions, mutations, putBytes };
}

function requireScanRequest(request: ZLinkStoreScanRequest): void {
  if (Buffer.byteLength(request.prefix, 'utf8') > 1_024) {
    throw new RangeError('Location Store scan prefix exceeds 1,024 UTF-8 bytes.');
  }
  if (!Number.isSafeInteger(request.limit) || request.limit < 1 || request.limit > 1_000) {
    throw new RangeError('Location Store scan limit must be in 1..1000.');
  }
  if (request.cursor !== undefined) requireCursor(request.cursor);
}

function requireKey(key: ZLinkStoreKey): string {
  const value = key.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 1_024) {
    throw new RangeError('Location Store key must contain 1..1,024 UTF-8 bytes.');
  }
  return value;
}

function requireMatchingKey(actual: string, expected: string): void {
  if (actual !== expected) {
    throw new Error('Redis opaque record key digest resolved to a different logical key.');
  }
}

function requireVersion(version: ZLinkStoreVersion): string {
  const value = version.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 4_096) {
    throw new RangeError('Location Store version must contain 1..4,096 UTF-8 bytes.');
  }
  return value;
}

function requireCursor(cursor: ZLinkStoreScanCursor): string {
  const value = cursor.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 4_096) {
    throw new RangeError('Location Store cursor must contain 1..4,096 UTF-8 bytes.');
  }
  return value;
}

function requireValue(bytes: Uint8Array, retentionMs: number | undefined): void {
  if (bytes.byteLength > MAX_VALUE_BYTES) {
    throw new RangeError('Location Store value exceeds 1 MiB.');
  }
  if (
    retentionMs !== undefined
    && (!Number.isSafeInteger(retentionMs) || retentionMs < 1)
  ) {
    throw new RangeError('Location Store retention must be a positive safe integer.');
  }
}

function parseCursor(cursor: ZLinkStoreScanCursor): {
  readonly snapshotId: string;
  readonly lastKey: string;
} {
  const value = requireCursor(cursor);
  const separator = value.lastIndexOf(':');
  const snapshotId = separator < 0 ? '' : value.slice(0, separator);
  const lastKeyHex = separator < 0 ? '' : value.slice(separator + 1);
  if (
    !/^[0-9a-f-]{36}$/.test(snapshotId)
    || !/^[0-9a-f]*$/.test(lastKeyHex)
    || lastKeyHex.length % 2 !== 0
  ) {
    throw new RangeError('Location Store scan cursor is invalid.');
  }
  return { snapshotId, lastKey: Buffer.from(lastKeyHex, 'hex').toString('utf8') };
}

function rawBytes(value: unknown): Uint8Array {
  if (Buffer.isBuffer(value)) return Uint8Array.from(value);
  if (value instanceof Uint8Array) return Uint8Array.from(value);
  return Uint8Array.from(Buffer.from(asString(value), 'utf8'));
}

function digest(value: string): string {
  return createHash('sha256').update(value, 'utf8').digest('hex');
}

function storeKey(value: string): ZLinkStoreKey {
  return { value } as ZLinkStoreKey;
}

function storeVersion(value: string): ZLinkStoreVersion {
  return { value } as ZLinkStoreVersion;
}

function scanCursor(value: string): ZLinkStoreScanCursor {
  return { value } as ZLinkStoreScanCursor;
}

function expiresAtOrUndefined(expiresAtMs: number): Date | undefined {
  return expiresAtMs === 0 ? undefined : fromUnixMs(expiresAtMs);
}

function fromUnixMs(value: number): Date {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error('Redis Store returned an invalid provider timestamp.');
  }
  return new Date(value);
}
