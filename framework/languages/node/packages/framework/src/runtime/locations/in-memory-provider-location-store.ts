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
  ZLINK_PROVIDER_MAX_SCAN_CURSOR_BYTES
} from '../../contracts/Locations/Stores';
import {
  addStoreCounter,
  readStoreCounter,
  sumStoreCounters,
  validateStoreWriteRequest
} from './provider-write-request';

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
    validateStoreWriteRequest(request);
    const storeNow = this.now();
    const counter = (key: string) => readStoreCounter(this.liveValue(key, storeNow, false)?.bytes);
    for (const condition of request.conditions) {
      if (condition.kind === 'sumAtMost') {
        const matched = sumStoreCounters(condition, counter);
        if (matched === undefined) return { kind: 'corrupt', storeNow, putVersions: [] };
        if (!matched) return { kind: 'conflict', storeNow };
        continue;
      }
      const current = this.liveValue(condition.key.value, storeNow, false);
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

    let added: Map<string, Uint8Array> | undefined;
    for (const mutation of request.mutations) {
      if (mutation.kind !== 'add') continue;
      const bytes = addStoreCounter(counter(mutation.key.value), mutation.delta);
      if (bytes === undefined) return { kind: 'corrupt', storeNow, putVersions: [] };
      added ??= new Map();
      added.set(mutation.key.value, bytes);
    }

    const putVersions = [];
    for (const mutation of request.mutations) {
      if (mutation.kind === 'delete') {
        this.values.delete(mutation.key.value);
        continue;
      }
      const retentionMs =
        mutation.kind === 'put' && mutation.retentionMs !== undefined
          ? Math.ceil(mutation.retentionMs)
          : undefined;
      const version = storeVersion((++this.nextVersion).toString());
      const expiresAt =
        retentionMs === undefined ? undefined : new Date(storeNow.getTime() + retentionMs);
      this.values.set(mutation.key.value, {
        bytes: mutation.kind === 'put' ? mutation.bytes.slice() : added!.get(mutation.key.value)!,
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

  private liveValue(key: string, storeNow: Date, pruneExpired = true): StoredValue | undefined {
    const value = this.values.get(key);
    if (value?.expiresAt !== undefined && value.expiresAt.getTime() <= storeNow.getTime()) {
      if (pruneExpired) this.values.delete(key);
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
