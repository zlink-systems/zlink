import { RequestResult } from '../backend/runtime-values';
import { ServiceWireFrameworkErrorCode } from '../foundation/service-wire-constants.generated';
import {
  decodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelay
} from '../foundation/service-stateful-wire-codec';
import { decodeMultipartParts, encodeMultipartPayload } from '../foundation/service-wire-m6a-codec';
import { isIndeterminateLocationStoreFailure } from './location-store-failure';
import { createHash } from 'node:crypto';
import type { ZLinkRelocationStore } from '../../contracts/Locations/RelocationStore';
import type { ZLinkAuthorityKey, ZLinkAuthoritySnapshot } from './internal-location-contracts';
import type { ZLinkAuthorityStore } from './internal-store-contracts';
import type { AuthorityRelocationCompletion } from './authority-relocation-record';
import { putNewRelocationBlob, relocationBlobReference } from './relocation-blob';
import { crc32c } from '../foundation/service-relocation-runtime';
import {
  encodeRelocationDataChunkV1,
  decodeRelocationDataChunkV1,
  encodeRelocationManifestV1,
  decodeRelocationManifestV1,
  type RelocationChunkEntryV1
} from '../protocol/service_wire_pilot_codec.generated';
import {
  completionDataLost,
  encodeRelocationCompletionRecord,
  decodeRelocationCompletionRecord,
  type RelocationCompletionEntry,
  type RelocationCompletionRecord,
  type RelocationCompletionDelivery
} from './relocation-completion-record';

const RETENTION_MS = 24 * 60 * 60 * 1000;
const CHUNK_BYTES = 64 * 1024 * 1024;
export const COMPLETION_MAX_BYTES = 256 * 1024 * 1024 * 1024;

/** Stateless persistence boundary used by the existing relocation authority owner. */
export class RelocationCompletionStore {
  constructor(
    private readonly authorities: ZLinkAuthorityStore,
    private readonly blobs: ZLinkRelocationStore
  ) {}

  async read(
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<RelocationCompletionRecord> {
    const relocation = authority.relocation;
    if (relocation?.completion === undefined)
      return completionDataLost('Completion authority is missing.');
    const completion = relocation.completion;
    if (completion.reference === null) {
      if (
        completion.completedResults !== 0n ||
        completion.pendingDeliveries !== 0n ||
        completion.sha256 !== null
      )
        completionDataLost('Empty completion counts disagree.');
      return { formatVersion: '1', relocationId: relocation.relocationId, entries: [] };
    }
    const root = await this.requireBlob(completion.reference, signal);
    let manifest: ReturnType<typeof decodeRelocationManifestV1>;
    try {
      manifest = decodeRelocationManifestV1(root.bytes);
    } catch (error) {
      if (!(error instanceof TypeError || error instanceof RangeError)) throw error;
      return completionDataLost('Completion manifest is corrupt.');
    }
    if (
      !Buffer.from(manifest.inventoryDigestSha256).equals(
        Buffer.from(relocation.sourceInventorySha256, 'hex')
      )
    )
      completionDataLost('Completion inventory digest differs.');
    if (
      manifest.chunks.length === 0 ||
      manifest.totalLength === 0n ||
      manifest.totalLength > BigInt(COMPLETION_MAX_BYTES)
    )
      completionDataLost('Invalid completion blob bounds.');
    const chunks: Buffer[] = [];
    for (const [index, entry] of manifest.chunks.entries()) {
      if (
        entry.order !== index ||
        entry.length > BigInt(CHUNK_BYTES) ||
        (index + 1 < manifest.chunks.length && entry.length !== BigInt(CHUNK_BYTES))
      )
        completionDataLost('Invalid completion chunk order or length.');
      const stored = await this.requireBlob(entry.reference, signal);
      let chunk: ReturnType<typeof decodeRelocationDataChunkV1>;
      try {
        chunk = decodeRelocationDataChunkV1(stored.bytes);
      } catch (error) {
        if (!(error instanceof TypeError || error instanceof RangeError)) throw error;
        return completionDataLost('Completion chunk is corrupt.');
      }
      if (
        chunk.order !== index ||
        BigInt(chunk.data.length) !== entry.length ||
        crc32c(chunk.data) !== entry.checksumCrc32c
      )
        completionDataLost('Completion chunk checksum differs.');
      chunks.push(Buffer.from(chunk.data));
      if (
        completion.pendingDeliveries > 0n &&
        stored.expiresAt.getTime() - stored.storeNow.getTime() <= RETENTION_MS / 2
      ) {
        if (
          (await this.blobs.renew(relocationBlobReference(entry.reference), RETENTION_MS, signal))
            .kind === 'missing'
        )
          completionDataLost('Completion chunk disappeared during renew.');
      }
    }
    const bytes = Buffer.concat(chunks);
    if (
      BigInt(bytes.length) !== manifest.totalLength ||
      crc32c(bytes) !== manifest.totalChecksumCrc32c ||
      sha256(bytes) !== completion.sha256
    )
      completionDataLost('Completion payload checksum differs.');
    const record = decodeRelocationCompletionRecord(bytes);
    if (
      record.relocationId !== relocation.relocationId ||
      BigInt(record.entries.length) !== completion.completedResults ||
      BigInt(record.entries.filter((e) => e.delivery === 'pending').length) !==
        completion.pendingDeliveries ||
      completion.completedResults > completion.acceptedRequests
    )
      completionDataLost('Completion identity or counts disagree.');
    if (
      completion.pendingDeliveries > 0n &&
      root.expiresAt.getTime() - root.storeNow.getTime() <= RETENTION_MS / 2
    ) {
      if (
        (
          await this.blobs.renew(
            relocationBlobReference(completion.reference),
            RETENTION_MS,
            signal
          )
        ).kind === 'missing'
      )
        completionDataLost('Completion manifest disappeared during renew.');
    }
    return record;
  }

  async append(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    entry: RelocationCompletionEntry,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const record = await this.read(expected, signal);
    const appended = completionEntryWithinBlobBound(record, entry);
    entry = appended.entry;
    const previous = record.entries.find((value) => sameCompletionIdentity(value, entry));
    if (previous !== undefined) {
      if (
        previous.reply !== entry.reply ||
        previous.replyRouteId !== entry.replyRouteId ||
        previous.objectKey !== entry.objectKey
      )
        completionDataLost('Completed request changed its result.');
      return expected;
    }
    if (BigInt(record.entries.length) >= expected.relocation!.completion!.acceptedRequests)
      completionDataLost('Completed results exceed accepted requests.');
    return this.replace(key, expected, appended.record, appended.bytes, signal);
  }

  async acknowledge(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    entry: RelocationCompletionEntry,
    delivery: Exclude<RelocationCompletionDelivery, 'pending'>,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    const record = await this.read(expected, signal);
    const previous = record.entries.find((value) => sameCompletionIdentity(value, entry));
    if (
      previous === undefined ||
      previous.replyRouteId !== entry.replyRouteId ||
      previous.objectKey !== entry.objectKey ||
      previous.reply !== entry.reply
    )
      completionDataLost('ACK does not match the stored request fence.');
    if (previous.delivery !== 'pending') return expected;
    const replacement = {
      ...record,
      entries: record.entries.map((value) => (value === previous ? { ...value, delivery } : value))
    };
    return this.replace(
      key,
      expected,
      replacement,
      encodeRelocationCompletionRecord(replacement),
      signal
    );
  }

  requireCompleted(authority: ZLinkAuthoritySnapshot): void {
    const completion = authority.relocation?.completion;
    if (
      completion === undefined ||
      completion.acceptedRequests !== completion.completedResults ||
      completion.pendingDeliveries !== 0n
    )
      completionDataLost('Relocation still has incomplete requests or pending delivery.');
  }

  private async replace(
    key: ZLinkAuthorityKey,
    expected: ZLinkAuthoritySnapshot,
    record: RelocationCompletionRecord,
    bytes: Buffer,
    signal?: AbortSignal
  ): Promise<ZLinkAuthoritySnapshot> {
    if (bytes.length > COMPLETION_MAX_BYTES)
      throw new RangeError('Completion payload exceeds the common blob bound.');
    const chunks: RelocationChunkEntryV1[] = [];
    for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
      const data = bytes.subarray(offset, offset + CHUNK_BYTES);
      const order = chunks.length;
      const reference = await this.putVerified(
        encodeRelocationDataChunkV1({ order, data }),
        signal
      );
      chunks.push({ order, reference, length: BigInt(data.length), checksumCrc32c: crc32c(data) });
    }
    const reference = await this.putVerified(
      encodeRelocationManifestV1({
        logicalFormatVersion: 1,
        totalLength: BigInt(bytes.length),
        totalChecksumCrc32c: crc32c(bytes),
        inventoryDigestSha256: Buffer.from(expected.relocation!.sourceInventorySha256, 'hex'),
        chunks
      }),
      signal
    );
    const completion: AuthorityRelocationCompletion = {
      acceptedRequests: expected.relocation!.completion!.acceptedRequests,
      completedResults: BigInt(record.entries.length),
      pendingDeliveries: BigInt(record.entries.filter((e) => e.delivery === 'pending').length),
      reference,
      sha256: sha256(bytes)
    };
    const mutation = {
      kind: 'put' as const,
      generationTransition: 'preserve' as const,
      payload: expected.payload,
      relocation: { ...expected.relocation!, completion }
    };
    const result = await this.authorities
      .compareExchangeAuthority(key, expected.storeVersion, mutation, signal)
      .catch(async (error: unknown) => {
        if (!isIndeterminateLocationStoreFailure(error, signal)) throw error;
        const observed = await this.authorities.readAuthority(key, signal);
        if (
          observed.kind === 'snapshot' &&
          observed.relocation?.relocationId === record.relocationId &&
          observed.relocation.completion?.reference === reference &&
          observed.ownerId === expected.ownerId &&
          observed.ownerLeaseGeneration === expected.ownerLeaseGeneration
        ) {
          await this.read(observed, signal);
          return { ...observed, kind: 'stored' as const };
        }
        throw error;
      });
    if (result.kind !== 'stored') throw new Error('Relocation completion CAS conflict.');
    return { ...result, kind: 'snapshot' };
  }

  private async putVerified(bytes: Uint8Array, signal?: AbortSignal): Promise<string> {
    const stored = await putNewRelocationBlob(this.blobs, bytes, RETENTION_MS, signal);
    const read = await this.requireBlob(stored.reference.value, signal);
    if (!Buffer.from(read.bytes).equals(Buffer.from(bytes)))
      completionDataLost('New completion blob differs after Put.');
    return stored.reference.value;
  }
  private async requireBlob(reference: string, signal?: AbortSignal) {
    const read = await this.blobs.read(relocationBlobReference(reference), signal);
    if (read.kind !== 'found') return completionDataLost('Published completion blob is missing.');
    return read;
  }
}
function sha256(bytes: Uint8Array): string {
  return createHash('sha256').update(bytes).digest('hex');
}
export function sameCompletionIdentity(
  left: RelocationCompletionEntry,
  right: RelocationCompletionEntry
): boolean {
  return (
    left.operationId === right.operationId &&
    left.source.ownerId === right.source.ownerId &&
    left.source.leaseGeneration === right.source.leaseGeneration &&
    left.source.nodeRid === right.source.nodeRid &&
    left.source.nodeGeneration === right.source.nodeGeneration
  );
}

/** Applies the common encoded-payload bound to the newly completed request only. */
export function completionEntryWithinBlobBound(
  record: RelocationCompletionRecord,
  entry: RelocationCompletionEntry,
  maxBytes = COMPLETION_MAX_BYTES
): { entry: RelocationCompletionEntry; record: RelocationCompletionRecord; bytes: Buffer } {
  const entries = record.entries.some((value) => sameCompletionIdentity(value, entry))
    ? record.entries.map((value) => (sameCompletionIdentity(value, entry) ? entry : value))
    : [...record.entries, entry];
  let replacement = { ...record, entries };
  let bytes = encodeRelocationCompletionRecord(replacement);
  if (bytes.length <= maxBytes) return { entry, record: replacement, bytes };
  const relay = decodeMaintenanceReplyRelay(
    decodeMultipartParts(Buffer.from(entry.reply, 'base64'), (part) => part)
  );
  const { payload: _payload, ...identity } = relay;
  const failed = {
    ...entry,
    reply: encodeMultipartPayload(
      encodeMaintenanceReplyRelay({
        ...identity,
        terminalResult: RequestResult.InternalError,
        failureCode: ServiceWireFrameworkErrorCode.requestFailed
      })
    ).toString('base64')
  };
  replacement = { ...record, entries: entries.map((value) => (value === entry ? failed : value)) };
  bytes = encodeRelocationCompletionRecord(replacement);
  return { entry: failed, record: replacement, bytes };
}
