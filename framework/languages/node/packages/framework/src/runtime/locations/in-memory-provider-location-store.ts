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
} from '../../contracts';
import {
  ZLINK_PROVIDER_MAX_KEY_BYTES,
  ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES,
  ZLINK_PROVIDER_MAX_PAGE_SIZE,
  ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES,
  ZLINK_PROVIDER_MAX_VALUE_BYTES,
  ZLINK_PROVIDER_MAX_VERSION_BYTES,
  ZLINK_PROVIDER_MAX_WRITE_BYTES,
  ZLINK_PROVIDER_MAX_WRITE_KEYS
} from '../../contracts/Locations/Stores';
export const PROVIDER_STORAGE_NAMESPACE_PREFIX = 'zlink:v11:';

interface StoredValue {
  readonly bytes: Uint8Array;
  readonly version: ZLinkStoreVersion;
  readonly expiresAt?: Date;
}

interface ScanSnapshot {
  readonly items: readonly { readonly key: ZLinkStoreKey; readonly value: StoredValue }[];
}

/**
 * Framework-owned primitive Store used by the explicit in-memory option.
 * Domain records are encoded by ZLinkLocationStoreRepository.
 */
export class ZLinkInMemoryProviderLocationStore implements ZLinkLocationStore {
  private readonly values = new Map<string, StoredValue>();
  private readonly scans = new Map<string, ScanSnapshot>();
  private nextVersion = 0n;
  private nextScan = 0n;

  constructor(private readonly now: () => Date = () => new Date()) {}

  async read(key: ZLinkStoreKey, signal?: AbortSignal): Promise<ZLinkStoreReadResult> {
    signal?.throwIfAborted();
    const storeNow = this.now();
    const value = this.liveValue(key.value, storeNow);
    if (value === undefined) return { kind: 'missing', storeNow };
    return {
      kind: 'found',
      value: {
        bytes: value.bytes.slice(),
        version: value.version,
        expiresAt: value.expiresAt,
        storeNow
      }
    };
  }

  async write(
    request: ZLinkStoreWriteRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreWriteResult> {
    signal?.throwIfAborted();
    requireWriteRequest(request);
    const storeNow = this.now();
    for (const condition of request.conditions) {
      const current = this.liveValue(condition.key.value, storeNow);
      if (condition.kind === 'missing') {
        if (current !== undefined) return { kind: 'conflict', storeNow };
      } else if (condition.kind === 'value') {
        if (
          current === undefined ||
          !Buffer.from(current.bytes).equals(Buffer.from(condition.expected))
        ) {
          return { kind: 'conflict', storeNow };
        }
      } else if (current?.version.value !== condition.expected.value) {
        return { kind: 'conflict', storeNow };
      }
    }

    const putVersions = [];
    for (const mutation of request.mutations) {
      if (mutation.kind === 'delete') {
        this.values.delete(mutation.key.value);
        continue;
      }
      const retentionMs =
        mutation.retentionMs === undefined ? undefined : Math.ceil(mutation.retentionMs);
      const version = storeVersion((++this.nextVersion).toString());
      const expiresAt =
        retentionMs === undefined ? undefined : new Date(storeNow.getTime() + retentionMs);
      this.values.set(mutation.key.value, {
        bytes: mutation.bytes.slice(),
        version,
        expiresAt
      });
      putVersions.push({ key: mutation.key, version });
    }
    return { kind: 'applied', putVersions, storeNow };
  }

  async scan(request: ZLinkStoreScanRequest, signal?: AbortSignal): Promise<ZLinkStoreScanResult> {
    signal?.throwIfAborted();
    requireScanRequest(request);
    const storeNow = this.now();
    let snapshotId: string;
    let offset: number;
    let snapshot: ScanSnapshot | undefined;
    if (request.cursor === undefined) {
      snapshotId = (++this.nextScan).toString();
      offset = 0;
      snapshot = {
        items: [...this.values.entries()]
          .filter(([key]) => key.startsWith(request.prefix))
          .map(([key, value]) => ({ key: storeKey(key), value }))
          .filter((item) => this.liveValue(item.key.value, storeNow) !== undefined)
          .sort((left, right) => left.key.value.localeCompare(right.key.value))
      };
      this.scans.set(snapshotId, snapshot);
    } else {
      const cursor = parseCursor(request.cursor);
      snapshotId = cursor?.[0] ?? '';
      offset = cursor?.[1] ?? 0;
      snapshot = this.scans.get(snapshotId);
      if (snapshot === undefined || offset > snapshot.items.length) return { kind: 'expired' };
    }

    let nextOffset = offset;
    let encodedBytes = 0;
    while (nextOffset < snapshot.items.length && nextOffset - offset < request.limit) {
      const item = snapshot.items[nextOffset];
      const itemBytes =
        Buffer.byteLength(item.key.value, 'utf8') +
        Buffer.byteLength(item.value.version.value, 'utf8') +
        item.value.bytes.byteLength;
      if (encodedBytes + itemBytes > ZLINK_PROVIDER_MAX_ENCODED_PAGE_BYTES) break;
      encodedBytes += itemBytes;
      nextOffset += 1;
    }
    const selected = snapshot.items.slice(offset, nextOffset);
    const nextCursor =
      nextOffset < snapshot.items.length ? scanCursor(`${snapshotId}:${nextOffset}`) : undefined;
    if (nextCursor === undefined) this.scans.delete(snapshotId);
    return {
      kind: 'page',
      value: {
        items: selected.map((item) => ({
          key: item.key,
          value: {
            bytes: item.value.bytes.slice(),
            version: item.value.version,
            expiresAt: item.value.expiresAt,
            storeNow
          }
        })),
        nextCursor,
        storeNow
      }
    };
  }

  private liveValue(key: string, storeNow: Date): StoredValue | undefined {
    const value = this.values.get(key);
    if (value?.expiresAt !== undefined && value.expiresAt.getTime() <= storeNow.getTime()) {
      this.values.delete(key);
      return undefined;
    }
    return value;
  }
}

export function storeKey(value: string): ZLinkStoreKey {
  return { value } as ZLinkStoreKey;
}

export function storeVersion(value: string): ZLinkStoreVersion {
  return { value } as ZLinkStoreVersion;
}

function scanCursor(value: string): ZLinkStoreScanCursor {
  return { value } as ZLinkStoreScanCursor;
}

function parseCursor(cursor: ZLinkStoreScanCursor): [string, number] | undefined {
  const separator = cursor.value.indexOf(':');
  const snapshotId = cursor.value.slice(0, separator);
  const encodedOffset = cursor.value.slice(separator + 1);
  const offset = Number(encodedOffset);
  if (separator < 1 || !/^(0|[1-9][0-9]*)$/.test(encodedOffset) || !Number.isSafeInteger(offset)) {
    return undefined;
  }
  return [snapshotId, offset];
}

function requireWriteRequest(request: ZLinkStoreWriteRequest): void {
  const conditionKeys = request.conditions.map((condition) => condition.key.value);
  const mutationKeys = request.mutations.map((mutation) => mutation.key.value);
  const keys = [...new Set([...conditionKeys, ...mutationKeys])];
  if (
    new Set(conditionKeys).size !== conditionKeys.length ||
    new Set(mutationKeys).size !== mutationKeys.length ||
    keys.length > ZLINK_PROVIDER_MAX_WRITE_KEYS
  ) {
    throw new RangeError(
      `Location Store write keys must be unique and bounded to ${ZLINK_PROVIDER_MAX_WRITE_KEYS.toLocaleString('en-US')}.`
    );
  }
  for (const key of keys) requireKey(key);
  const encodedSize =
    request.conditions.reduce((sum, condition) => {
      const keyBytes = Buffer.byteLength(condition.key.value, 'utf8');
      if (condition.kind === 'missing') return sum + keyBytes;
      if (condition.kind === 'value') {
        requireValue(condition.expected, undefined);
        return sum + keyBytes + condition.expected.byteLength;
      }
      const versionBytes = Buffer.byteLength(condition.expected.value, 'utf8');
      if (versionBytes < 1 || versionBytes > ZLINK_PROVIDER_MAX_VERSION_BYTES) {
        throw new RangeError(
          `Location Store version must contain 1..${ZLINK_PROVIDER_MAX_VERSION_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
        );
      }
      return sum + keyBytes + versionBytes;
    }, 0) +
    request.mutations.reduce((sum, mutation) => {
      const keyBytes = Buffer.byteLength(mutation.key.value, 'utf8');
      if (mutation.kind !== 'put') return sum + keyBytes;
      requireValue(mutation.bytes, mutation.retentionMs);
      return sum + keyBytes + mutation.bytes.byteLength;
    }, 0);
  if (encodedSize > ZLINK_PROVIDER_MAX_WRITE_BYTES) {
    throw new RangeError(
      `Location Store write exceeds ${ZLINK_PROVIDER_MAX_WRITE_BYTES / (1024 * 1024)} MiB.`
    );
  }
}

function requireScanRequest(request: ZLinkStoreScanRequest): void {
  if (request.cursor !== undefined) {
    const bytes = Buffer.byteLength(request.cursor.value, 'utf8');
    if (bytes < 1 || bytes > ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES) {
      throw new RangeError(
        `Location Store cursor must contain 1..${ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
      );
    }
  }
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
}

function requireValue(bytes: Uint8Array, retentionMs: number | undefined): number | undefined {
  if (bytes.byteLength > ZLINK_PROVIDER_MAX_VALUE_BYTES) {
    throw new RangeError(
      `Location Store value exceeds ${ZLINK_PROVIDER_MAX_VALUE_BYTES / (1024 * 1024)} MiB.`
    );
  }
  if (
    retentionMs !== undefined &&
    (!Number.isFinite(retentionMs) ||
      retentionMs <= 0 ||
      !Number.isSafeInteger(Math.ceil(retentionMs)))
  ) {
    throw new RangeError('Location Store retention must round to a positive safe integer.');
  }
  return retentionMs === undefined ? undefined : Math.ceil(retentionMs);
}

function requireKey(value: string): void {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > ZLINK_PROVIDER_MAX_KEY_BYTES) {
    throw new RangeError(
      `Location Store key must contain 1..${ZLINK_PROVIDER_MAX_KEY_BYTES.toLocaleString('en-US')} UTF-8 bytes.`
    );
  }
}
