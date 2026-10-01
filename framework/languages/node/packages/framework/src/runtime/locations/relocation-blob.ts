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
  let reference = relocationBlobReference(randomUUID());
  for (; ; await macrotaskBoundary()) {
    signal?.throwIfAborted();
    let result: ZLinkBlobPutResult;
    try {
      result = await awaitWithAbort(store.put(reference, payload, retentionMs, signal), signal);
    } catch {
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
