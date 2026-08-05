import { randomUUID } from 'node:crypto';
import type {
  ZLinkBlobReference,
  ZLinkRelocationStore
} from '../../contracts/Locations/RelocationStore';

const MAX_REFERENCE_GENERATION_ATTEMPTS = 4;

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
  for (let attempt = 0; attempt < MAX_REFERENCE_GENERATION_ATTEMPTS; attempt += 1) {
    signal?.throwIfAborted();
    const reference = relocationBlobReference(randomUUID());
    const result = await store.put(reference, payload, retentionMs, signal);
    if (result.kind !== 'conflict') {
      return {
        reference,
        expiresAt: result.expiresAt,
        storeNow: result.storeNow
      };
    }
  }
  throw new Error('Unable to allocate a unique Relocation Store reference.');
}
