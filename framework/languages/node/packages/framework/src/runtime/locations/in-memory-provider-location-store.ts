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
      requireValue(mutation.bytes, mutation.retentionMs);
      const version = storeVersion((++this.nextVersion).toString());
      const expiresAt = mutation.retentionMs === undefined
        ? undefined
        : new Date(storeNow.getTime() + mutation.retentionMs);
      this.values.set(mutation.key.value, {
        bytes: mutation.bytes.slice(),
        version,
        expiresAt
      });
      putVersions.push({ key: mutation.key, version });
    }
    return { kind: 'applied', putVersions, storeNow };
  }

  async scan(
    request: ZLinkStoreScanRequest,
    signal?: AbortSignal
  ): Promise<ZLinkStoreScanResult> {
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
          .filter(item => this.liveValue(item.key.value, storeNow) !== undefined)
          .sort((left, right) => left.key.value.localeCompare(right.key.value))
      };
      this.scans.set(snapshotId, snapshot);
    } else {
      [snapshotId, offset] = parseCursor(request.cursor);
      snapshot = this.scans.get(snapshotId);
      if (snapshot === undefined) return { kind: 'expired' };
    }

    const selected = snapshot.items.slice(offset, offset + request.limit);
    const nextOffset = offset + selected.length;
    const nextCursor = nextOffset < snapshot.items.length
      ? scanCursor(`${snapshotId}:${nextOffset}`)
      : undefined;
    if (nextCursor === undefined) this.scans.delete(snapshotId);
    return {
      kind: 'page',
      value: {
        items: selected.map(item => ({
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

function parseCursor(cursor: ZLinkStoreScanCursor): [string, number] {
  const separator = cursor.value.indexOf(':');
  const snapshotId = cursor.value.slice(0, separator);
  const offset = Number(cursor.value.slice(separator + 1));
  if (separator < 1 || !Number.isSafeInteger(offset) || offset < 0) {
    throw new RangeError('Location Store scan cursor is invalid.');
  }
  return [snapshotId, offset];
}

function requireWriteRequest(request: ZLinkStoreWriteRequest): void {
  const conditionKeys = request.conditions.map(condition => condition.key.value);
  const mutationKeys = request.mutations.map(mutation => mutation.key.value);
  const keys = [...new Set([...conditionKeys, ...mutationKeys])];
  if (new Set(conditionKeys).size !== conditionKeys.length
    || new Set(mutationKeys).size !== mutationKeys.length
    || keys.length > 2_048) {
    throw new RangeError('Location Store write keys must be unique and bounded to 2,048.');
  }
  for (const key of keys) requireKey(key);
  const encodedSize = request.mutations.reduce(
    (sum, mutation) => sum + (mutation.kind === 'put' ? mutation.bytes.byteLength : 0),
    0
  );
  if (encodedSize > 4 * 1024 * 1024) {
    throw new RangeError('Location Store write exceeds 4 MiB.');
  }
}

function requireScanRequest(request: ZLinkStoreScanRequest): void {
  if (Buffer.byteLength(request.prefix, 'utf8') > 1_024) {
    throw new RangeError('Location Store scan prefix exceeds 1,024 UTF-8 bytes.');
  }
  if (!Number.isSafeInteger(request.limit) || request.limit < 1 || request.limit > 1_000) {
    throw new RangeError('Location Store scan limit must be in 1..1000.');
  }
}

function requireValue(bytes: Uint8Array, retentionMs: number | undefined): void {
  if (bytes.byteLength > 1024 * 1024) {
    throw new RangeError('Location Store value exceeds 1 MiB.');
  }
  if (retentionMs !== undefined && (!Number.isSafeInteger(retentionMs) || retentionMs < 1)) {
    throw new RangeError('Location Store retention must be a positive safe integer.');
  }
}

function requireKey(value: string): void {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 1_024) {
    throw new RangeError('Location Store key must contain 1..1,024 UTF-8 bytes.');
  }
}
