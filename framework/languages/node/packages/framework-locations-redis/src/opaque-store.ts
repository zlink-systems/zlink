import {
  STORE_COUNTER_MAX,
  ZLINK_PROVIDER_MAX_KEY_BYTES,
  ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES,
  ZLINK_PROVIDER_MAX_PAGE_SIZE,
  validateStoreWriteRequest
} from './framework-internal';
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
  REDIS_STORE_TOKEN,
  OPAQUE_READ_SCRIPT,
  OPAQUE_SCAN_CONTINUE_SCRIPT,
  OPAQUE_SCAN_START_SCRIPT,
  OPAQUE_WRITE_SCRIPT
} from './opaque-redis-scripts';
import { asArray, asString, toNumber } from './redis-values';

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

  async read(key: ZLinkStoreKey, signal?: AbortSignal): Promise<ZLinkStoreReadResult> {
    const logicalKey = requireKey(key);
    const result = asArray(
      await this.connection.eval(OPAQUE_READ_SCRIPT, [this.rowKey(logicalKey)], [], signal)
    );
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
    const result = asArray(
      await this.connection.eval(
        OPAQUE_WRITE_SCRIPT,
        [
          this.indexKey(),
          this.mapKey(),
          this.cleanupKey(),
          this.sequenceKey(),
          this.snapshotExpiryKey(),
          this.snapshotBoundaryKey(),
          ...encoded.keys.map((key) => this.rowKey(key))
        ],
        [
          JSON.stringify(encoded.conditions),
          JSON.stringify(encoded.mutations),
          ...encoded.expectedBytes,
          ...encoded.putBytes
        ],
        signal
      )
    );
    const storeNow = fromUnixMs(toNumber(result[1]));
    const outcome = asString(result[0]);
    if (outcome === REDIS_STORE_TOKEN.Corrupt)
      return { kind: 'corrupt', storeNow, putVersions: [] };
    if (outcome === REDIS_STORE_TOKEN.Conflict) return { kind: 'conflict', storeNow };
    if (outcome === REDIS_STORE_TOKEN.Backlog) {
      throw new Error('Redis Location Store version backlog is full.');
    }
    if (outcome !== REDIS_STORE_TOKEN.Applied) {
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

  async scan(request: ZLinkStoreScanRequest, signal?: AbortSignal): Promise<ZLinkStoreScanResult> {
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
    if (cursor === undefined) {
      return { kind: 'expired' };
    }
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

  private async readScanPage(snapshotId: string, raw: unknown): Promise<ZLinkStoreScanResult> {
    const result = asArray(raw);
    const outcome = asString(result[0]);
    if (outcome === REDIS_STORE_TOKEN.Expired) return { kind: 'expired' };
    if (outcome === REDIS_STORE_TOKEN.Capacity) {
      throw new Error('Redis Location Store snapshot capacity is full.');
    }
    if (outcome !== REDIS_STORE_TOKEN.Page) {
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
        nextCursor:
          nextKey.length === 0
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
  readonly expectedBytes: readonly Buffer[];
  readonly putBytes: readonly Buffer[];
}

function encodeWrite(request: ZLinkStoreWriteRequest): EncodedWrite {
  const keys = [...validateStoreWriteRequest(request)];
  // Row keys are appended after the six fixed auxiliary keys; the script
  // adds 6 to this 1-based index before indexing into KEYS.
  const keyIndex = new Map(keys.map((key, index) => [key, index + 1]));
  const expectedBytes: Buffer[] = [];
  const conditions = request.conditions.map((condition) => {
    if (condition.kind === 'sumAtMost') {
      const threshold = condition.limit - condition.delta;
      const maxSum = BigInt(condition.keys.length) * STORE_COUNTER_MAX;
      return [
        REDIS_STORE_TOKEN.SumAtMost,
        condition.keys.map((key) => keyIndex.get(key.value)),
        clamp(threshold, -1n, maxSum).toString()
      ];
    }
    const key = condition.key.value;
    if (condition.kind === 'missing') return [REDIS_STORE_TOKEN.Missing, keyIndex.get(key), key];
    if (condition.kind === 'value') {
      expectedBytes.push(Buffer.from(condition.expected));
      return [REDIS_STORE_TOKEN.Value, keyIndex.get(key), key, expectedBytes.length + 2];
    }
    return [REDIS_STORE_TOKEN.Version, keyIndex.get(key), key, condition.expected.value];
  });
  const putBytes: Buffer[] = [];
  const mutations = request.mutations.map((mutation) => {
    const key = mutation.key.value;
    if (mutation.kind === 'delete') return [REDIS_STORE_TOKEN.Delete, keyIndex.get(key), key];
    if (mutation.kind === 'add') {
      const overflow = STORE_COUNTER_MAX + 1n;
      return [
        REDIS_STORE_TOKEN.Add,
        keyIndex.get(key),
        key,
        clamp(mutation.delta, -overflow, overflow).toString()
      ];
    }
    putBytes.push(Buffer.from(mutation.bytes));
    const retentionMs =
      mutation.retentionMs === undefined ? false : Math.ceil(mutation.retentionMs);
    return [REDIS_STORE_TOKEN.Put, keyIndex.get(key), key, retentionMs];
  });
  return { keys, conditions, mutations, expectedBytes, putBytes };
}

function clamp(value: bigint, min: bigint, max: bigint): bigint {
  if (value < min) return min;
  return value > max ? max : value;
}

function requireScanRequest(request: ZLinkStoreScanRequest): void {
  if (Buffer.byteLength(request.prefix, 'utf8') > ZLINK_PROVIDER_MAX_KEY_BYTES) {
    throw new RangeError(
      `Location Store scan prefix exceeds ${ZLINK_PROVIDER_MAX_KEY_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
    );
  }
  if (
    !Number.isSafeInteger(request.limit) ||
    request.limit < 1 ||
    request.limit > ZLINK_PROVIDER_MAX_PAGE_SIZE
  ) {
    throw new RangeError(
      `Location Store scan limit must be in 1..${ZLINK_PROVIDER_MAX_PAGE_SIZE}.`
    );
  }
  if (request.cursor !== undefined) requireCursor(request.cursor);
}

function requireKey(key: ZLinkStoreKey): string {
  const value = key.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > ZLINK_PROVIDER_MAX_KEY_BYTES) {
    throw new RangeError(
      `Location Store key must contain 1..${ZLINK_PROVIDER_MAX_KEY_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
    );
  }
  return value;
}

function requireMatchingKey(actual: string, expected: string): void {
  if (actual !== expected) {
    throw new Error('Redis opaque record key digest resolved to a different logical key.');
  }
}

function requireCursor(cursor: ZLinkStoreScanCursor): string {
  const value = cursor.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES) {
    throw new RangeError(
      `Location Store cursor must contain 1..${ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
    );
  }
  return value;
}

function parseCursor(cursor: ZLinkStoreScanCursor):
  | {
      readonly snapshotId: string;
      readonly lastKey: string;
    }
  | undefined {
  const value = cursor.value;
  const separator = value.lastIndexOf(':');
  const snapshotId = separator < 0 ? '' : value.slice(0, separator);
  const lastKeyHex = separator < 0 ? '' : value.slice(separator + 1);
  if (
    !/^[0-9a-f-]{36}$/.test(snapshotId) ||
    !/^[0-9a-f]*$/.test(lastKeyHex) ||
    lastKeyHex.length % 2 !== 0
  ) {
    return undefined;
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
