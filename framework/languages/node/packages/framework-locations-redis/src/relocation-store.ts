import { createHash } from 'node:crypto';
import type {
  ZLinkBlobPutResult,
  ZLinkBlobReadResult,
  ZLinkBlobReference,
  ZLinkBlobRenewResult,
  ZLinkRelocationStore
} from '@zlink-systems/framework';
import type { ZLinkRedisRelocationOptions } from './redis-options';
import { RedisConnection } from './redis-connection';
import {
  BLOB_PUT_SCRIPT,
  BLOB_READ_SCRIPT,
  BLOB_RENEW_SCRIPT
} from './opaque-redis-scripts';
import { asArray, asString, toNumber } from './redis-values';

const MAX_BLOB_BYTES = 64 * 1024 * 1024;

/** Redis implementation of immutable relocation blob storage. */
export class ZLinkRedisRelocationStore implements ZLinkRelocationStore {
  private readonly connection: RedisConnection;
  private readonly domain: string;

  constructor(options: ZLinkRedisRelocationOptions) {
    this.connection = new RedisConnection(options);
    this.domain = `${options.keyPrefix}:{zlink-relocation-opaque-v1}`;
  }

  async put(
    reference: ZLinkBlobReference,
    payload: Uint8Array,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobPutResult> {
    const referenceValue = requireReference(reference);
    requirePayload(payload);
    const retention = requireRetention(retentionMs);
    const result = asArray(await this.connection.eval(
      BLOB_PUT_SCRIPT,
      [this.blobKey(referenceValue)],
      [referenceValue, Buffer.from(payload), String(retention)],
      signal
    ));
    const kind = asString(result[0]);
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (kind === 'conflict') return { kind, storeNow };
    return {
      kind: kind === 'alreadyStored' ? 'alreadyStored' : 'stored',
      storeNow,
      expiresAt: fromUnixMs(toNumber(result[2]))
    };
  }

  async read(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<ZLinkBlobReadResult> {
    const referenceValue = requireReference(reference);
    const result = asArray(await this.connection.eval(
      BLOB_READ_SCRIPT,
      [this.blobKey(referenceValue)],
      [referenceValue],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (toNumber(result[0]) !== 1) return { kind: 'missing', storeNow };
    return {
      kind: 'found',
      bytes: Uint8Array.from(asBuffer(result[2])),
      expiresAt: fromUnixMs(toNumber(result[3])),
      storeNow
    };
  }

  async renew(
    reference: ZLinkBlobReference,
    retentionMs: number,
    signal?: AbortSignal
  ): Promise<ZLinkBlobRenewResult> {
    const referenceValue = requireReference(reference);
    const retention = requireRetention(retentionMs);
    const result = asArray(await this.connection.eval(
      BLOB_RENEW_SCRIPT,
      [this.blobKey(referenceValue)],
      [referenceValue, String(retention)],
      signal
    ));
    const storeNow = fromUnixMs(toNumber(result[1]));
    if (toNumber(result[0]) !== 1) return { kind: 'missing', storeNow };
    return {
      kind: 'renewed',
      expiresAt: fromUnixMs(toNumber(result[2])),
      storeNow
    };
  }

  async delete(
    reference: ZLinkBlobReference,
    signal?: AbortSignal
  ): Promise<void> {
    const referenceValue = requireReference(reference);
    await this.connection.command(['DEL', this.blobKey(referenceValue)], signal);
  }

  async dispose(): Promise<void> {
    await this.connection.dispose();
  }

  private blobKey(reference: string): string {
    return `${this.domain}:blob:${digest(reference)}`;
  }
}

function requireReference(reference: ZLinkBlobReference): string {
  const value = reference.value;
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 4_096) {
    throw new RangeError('Relocation Store reference must contain 1..4,096 UTF-8 bytes.');
  }
  return value;
}

function requirePayload(payload: Uint8Array): void {
  if (payload.byteLength > MAX_BLOB_BYTES) {
    throw new RangeError('Relocation Store blob exceeds 64 MiB.');
  }
}

function requireRetention(retentionMs: number): number {
  if (!Number.isSafeInteger(retentionMs) || retentionMs < 1) {
    throw new RangeError('Relocation Store retention must be a positive safe integer.');
  }
  return retentionMs;
}

function asBuffer(value: unknown): Buffer {
  if (Buffer.isBuffer(value)) return Buffer.from(value);
  if (value instanceof Uint8Array) return Buffer.from(value);
  return Buffer.from(asString(value), 'utf8');
}

function digest(value: string): string {
  return createHash('sha256').update(value, 'utf8').digest('hex');
}

function fromUnixMs(value: number): Date {
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new Error('Redis Store returned an invalid provider timestamp.');
  }
  return new Date(value);
}
