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

/** Redis implementation of the opaque Location Store provider SPI. */
export class ZLinkRedisLocationStore implements ZLinkLocationStore {
  private readonly connection: RedisConnection;
  private readonly domain: string;

  constructor(options: ZLinkRedisLocationOptions) {
    this.connection = new RedisConnection(options);
    this.domain = `${options.keyPrefix}:{zlink-location-opaque-v1}`;
  }

  async read(
    key: ZLinkStoreKey,
    signal?: AbortSignal
  ): Promise<ZLinkStoreReadResult> {
    const logicalKey = requireKey(key);
    const result = asArray(await this.connection.eval(
      OPAQUE_READ_SCRIPT,
      [this.rowKey(logicalKey), this.indexKey()],
      [logicalKey],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (toNumber(result[0]) !== 1) return { kind: 'missing', storeNow };
    const expiresAtMs = numberOrZero(result[4]);
    return {
      kind: 'found',
      value: {
        bytes: decodeBytes(result[2]),
        version: storeVersion(asString(result[3])),
        expiresAt: expiresAtMs === 0 ? undefined : fromUnixMs(expiresAtMs),
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
        this.versionKey(),
        ...encoded.keys.map(key => this.rowKey(key))
      ],
      [JSON.stringify(encoded.conditions), JSON.stringify(encoded.mutations)],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (asString(result[0]) === 'conflict') return { kind: 'conflict', storeNow };
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
        0,
        request.limit,
        await this.connection.eval(
          OPAQUE_SCAN_START_SCRIPT,
          [this.indexKey(), this.snapshotKey(snapshotId)],
          [request.prefix, String(request.limit)],
          signal
        )
      );
    }
    const cursor = parseCursor(request.cursor);
    return await this.readScanPage(
      cursor.snapshotId,
      cursor.offset,
      request.limit,
      await this.connection.eval(
        OPAQUE_SCAN_CONTINUE_SCRIPT,
        [this.snapshotKey(cursor.snapshotId)],
        [String(cursor.offset), String(request.limit)],
        signal
      )
    );
  }

  async dispose(): Promise<void> {
    await this.connection.dispose();
  }

  private async readScanPage(
    snapshotId: string,
    _offset: number,
    _limit: number,
    raw: unknown
  ): Promise<ZLinkStoreScanResult> {
    const result = asArray(raw);
    if (toNumber(result[0]) !== 1) return { kind: 'expired' };
    const storeNow = fromUnixMs(toNumber(result[1]));
    const nextOffset = asString(result[2]);
    const items = [];
    for (let index = 3; index < result.length; index += 2) {
      const key = asString(result[index]);
      const record = asArray(JSON.parse(asString(result[index + 1])));
      const expiresAtMs = numberOrZero(record[2]);
      items.push({
        key: storeKey(key),
        value: {
          bytes: decodeBase64(asString(record[0])),
          version: storeVersion(asString(record[1])),
          expiresAt: expiresAtMs === 0 ? undefined : fromUnixMs(expiresAtMs),
          storeNow
        }
      });
    }
    return {
      kind: 'page',
      value: {
        items,
        nextCursor: nextOffset.length === 0
          ? undefined
          : scanCursor(`${snapshotId}:${nextOffset}`),
        storeNow
      }
    };
  }

  private indexKey(): string {
    return `${this.domain}:index`;
  }

  private versionKey(): string {
    return `${this.domain}:version`;
  }

  private rowKey(logicalKey: string): string {
    return `${this.domain}:record:${digest(logicalKey)}`;
  }

  private snapshotKey(snapshotId: string): string {
    return `${this.domain}:scan:${snapshotId}`;
  }
}

interface EncodedWrite {
  readonly keys: readonly string[];
  readonly conditions: readonly unknown[];
  readonly mutations: readonly unknown[];
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
  const keyIndex = new Map(keys.map((key, index) => [key, index + 3]));
  let encodedBytes = 0;
  const conditions = request.conditions.map(condition => {
    const key = requireKey(condition.key);
    encodedBytes += Buffer.byteLength(key, 'utf8');
    if (condition.kind === 'missing') return ['missing', keyIndex.get(key), key];
    const expected = requireVersion(condition.expected);
    encodedBytes += Buffer.byteLength(expected, 'utf8');
    return ['version', keyIndex.get(key), key, expected];
  });
  const mutations = request.mutations.map(mutation => {
    const key = requireKey(mutation.key);
    encodedBytes += Buffer.byteLength(key, 'utf8');
    if (mutation.kind === 'delete') return ['delete', keyIndex.get(key), key];
    requireValue(mutation.bytes, mutation.retentionMs);
    encodedBytes += mutation.bytes.byteLength;
    return [
      'put',
      keyIndex.get(key),
      key,
      Buffer.from(mutation.bytes).toString('base64'),
      mutation.retentionMs ?? false
    ];
  });
  if (encodedBytes > MAX_WRITE_BYTES) {
    throw new RangeError('Location Store write exceeds 4 MiB encoded input.');
  }
  return { keys, conditions, mutations };
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
  readonly offset: number;
} {
  const value = requireCursor(cursor);
  const separator = value.lastIndexOf(':');
  const snapshotId = value.slice(0, separator);
  const offset = Number(value.slice(separator + 1));
  if (
    !/^[0-9a-f-]{36}$/.test(snapshotId)
    || !Number.isSafeInteger(offset)
    || offset < 0
  ) {
    throw new RangeError('Location Store scan cursor is invalid.');
  }
  return { snapshotId, offset };
}

function decodeBytes(value: unknown): Uint8Array {
  return decodeBase64(asString(value));
}

function decodeBase64(encoded: string): Uint8Array {
  const bytes = Buffer.from(encoded, 'base64');
  if (bytes.toString('base64') !== encoded) {
    throw new Error('Redis Location Store contains non-canonical bytes.');
  }
  return Uint8Array.from(bytes);
}

function numberOrZero(value: unknown): number {
  if (value === '' || value === null || value === undefined) return 0;
  return toNumber(value);
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

function fromUnixMs(value: number): Date {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error('Redis Store returned an invalid provider timestamp.');
  }
  return new Date(value);
}
