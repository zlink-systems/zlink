import {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} from '../../contracts/Errors/ZLinkFrameworkException';
import { isIndeterminateLocationStoreFailure } from './location-store-failure';
import type { ZLinkRuntimeMetrics } from '../diagnostics/runtime-metrics';
import { randomUUID } from 'node:crypto';
import { awaitWithAbort, macrotaskBoundary } from '../abort';
import type {
  ZLinkBlobReference,
  ZLinkBlobPutResult,
  ZLinkRelocationStore
} from '../../contracts/Locations/RelocationStore';

export function relocationBlobReference(value: string): ZLinkBlobReference {
  return { value } as ZLinkBlobReference;
}

export async function putNewRelocationBlob(
  store: ZLinkRelocationStore,
  payload: Uint8Array,
  retentionMs: number,
  signal?: AbortSignal
): Promise<{
  readonly reference: ZLinkBlobReference;
  readonly expiresAt: Date;
  readonly storeNow: Date;
}> {
  if (store instanceof ZLinkProviderRelocationRepository) {
    return store.putNew(payload, retentionMs, signal);
  }
  return putNewRelocationBlobCore(store, payload, retentionMs, signal);
}

async function putNewRelocationBlobCore(
  store: ZLinkRelocationStore,
  payload: Uint8Array,
  retentionMs: number,
  signal?: AbortSignal
): Promise<{
  readonly reference: ZLinkBlobReference;
  readonly expiresAt: Date;
  readonly storeNow: Date;
}> {
  let reference = relocationBlobReference(randomUUID());
  for (; ; await macrotaskBoundary()) {
    signal?.throwIfAborted();
    let result: ZLinkBlobPutResult;
    try {
      result = await awaitWithAbort(store.put(reference, payload, retentionMs, signal), signal);
    } catch (error) {
      if (!isRelocationProviderFailure(error)) throw error;
      signal?.throwIfAborted();
      const read = await awaitWithAbort(store.read(reference, signal), signal);
      if (read.kind === 'found' && Buffer.compare(read.bytes, payload) === 0) {
        return { reference, expiresAt: read.expiresAt, storeNow: read.storeNow };
      }
      if (read.kind === 'missing') continue;
      result = { kind: 'conflict', storeNow: read.storeNow };
    }
    if (result.kind !== 'conflict') {
      return {
        reference,
        expiresAt: result.expiresAt,
        storeNow: result.storeNow
      };
    }
    reference = relocationBlobReference(randomUUID());
  }
}

/** Observes provider requests before callers interpret or decode blob contents. */
export class ZLinkProviderRelocationRepository implements ZLinkRelocationStore {
  constructor(
    private readonly provider: ZLinkRelocationStore,
    private readonly metrics: ZLinkRuntimeMetrics
  ) {}

  putNew(payload: Uint8Array, retentionMs: number, signal?: AbortSignal) {
    return this.observe(
      'relocation_put',
      () => putNewRelocationBlobCore(this.provider, payload, retentionMs, signal),
      signal
    );
  }
  put(...args: Parameters<ZLinkRelocationStore['put']>) {
    return this.observe('relocation_put', () => this.provider.put(...args), args[3]);
  }
  read(...args: Parameters<ZLinkRelocationStore['read']>) {
    return this.observe('relocation_get', () => this.provider.read(...args), args[1]);
  }
  renew(...args: Parameters<ZLinkRelocationStore['renew']>) {
    return this.observe('relocation_put', () => this.provider.renew(...args), args[2]);
  }
  delete(...args: Parameters<ZLinkRelocationStore['delete']>) {
    return this.observe('relocation_delete', () => this.provider.delete(...args), args[1]);
  }
  private async observe<T>(
    operation: 'relocation_put' | 'relocation_get' | 'relocation_delete',
    request: () => Promise<T>,
    signal?: AbortSignal
  ): Promise<T> {
    try {
      return await request();
    } catch (error) {
      if (isRelocationProviderFailure(error, signal)) {
        this.metrics.recordLocationStoreError(operation);
      }
      throw error;
    }
  }
}

function isRelocationProviderFailure(error: unknown, signal?: AbortSignal): boolean {
  return (
    isIndeterminateLocationStoreFailure(error, signal) &&
    !(
      error instanceof ZLinkFrameworkException && error.kind !== ZLinkFrameworkErrorKind.Unavailable
    )
  );
}
