import { RequestResult } from '../backend/runtime-values';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts/Errors/ZLinkFrameworkException';
import { ServiceWireFrameworkErrorCode } from '../foundation/service-wire-constants.generated';
import {
  decodeMaintenanceReplyRelay,
  encodeMaintenanceReplyRelay,
  type ServiceMaintenanceReplyRelay
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

/** The CAS and its readback both lost their results. */
export class CompletionPublicationUncertain extends Error {
  constructor(
    readonly candidate: ZLinkAuthoritySnapshot,
    cause: unknown
  ) {
    super('Completion publication could not be read back.', { cause });
  }
}

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
    signal?.throwIfAborted();
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
    const manifest = completionManifest(root.bytes, relocation.sourceInventorySha256);
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
        signal?.throwIfAborted();
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
      signal?.throwIfAborted();
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
    const accepted = expected.relocation!.completion!.acceptedRequests;
    const previous = record.entries.find((value) => sameCompletionIdentity(value, entry));
    if (previous !== undefined) {
      if (
        previous.replyRouteId !== entry.replyRouteId ||
        previous.objectKey !== entry.objectKey ||
        (previous.reply !== entry.reply && previous.reply !== failedEntry(entry).reply)
      )
        completionDataLost('Completed request changed its result.');
      return expected;
    }
    if (BigInt(record.entries.length) >= accepted)
      completionDataLost('Completed results exceed accepted requests.');
    const appended = completionEntryWithinBlobBound(record, entry, COMPLETION_MAX_BYTES, accepted);
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

  async retainIfPending(
    authority: ZLinkAuthoritySnapshot | undefined,
    signal?: AbortSignal
  ): Promise<void> {
    if (
      authority === undefined ||
      (authority.relocation?.completion?.pendingDeliveries ?? 0n) === 0n
    )
      return;
    try {
      await this.read(authority, signal);
    } catch (error) {
      // Only a current authority read can diagnose loss of a possibly superseded blob.
      if (!(
        error instanceof ZLinkFrameworkException && error.kind === ZLinkFrameworkErrorKind.DataLost
      ))
        throw error;
    }
  }

  async deleteUnpublished(authority: ZLinkAuthoritySnapshot, signal?: AbortSignal): Promise<void> {
    const relocation = authority.relocation;
    const reference = relocation?.completion?.reference;
    if (relocation === undefined || reference == null) return;
    const root = relocationBlobReference(reference);
    signal?.throwIfAborted();
    const stored = await this.blobs.read(root, signal);
    if (stored.kind === 'missing') return;
    const manifest = completionManifest(stored.bytes, relocation.sourceInventorySha256);
    for (const chunk of manifest.chunks) {
      signal?.throwIfAborted();
      await this.blobs.delete(relocationBlobReference(chunk.reference), signal);
    }
    signal?.throwIfAborted();
    await this.blobs.delete(root, signal);
  }

  requireCompleted(authority: ZLinkAuthoritySnapshot): void {
    if (!isRelocationCompletionReady(authority))
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
    signal?.throwIfAborted();
    const result = await this.authorities
      .compareExchangeAuthority(key, expected.storeVersion, mutation, signal)
      .catch(async (error: unknown) => {
        if (!isIndeterminateLocationStoreFailure(error, signal)) throw error;
        const observed = await this.authorities
          .readAuthority(key, signal)
          .catch((readError: unknown) => {
            if (!isIndeterminateLocationStoreFailure(readError, signal)) throw readError;
            throw new CompletionPublicationUncertain(
              { ...expected, relocation: mutation.relocation },
              readError
            );
          });
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
    signal?.throwIfAborted();
    const read = await this.blobs.read(relocationBlobReference(reference), signal);
    if (read.kind !== 'found') return completionDataLost('Published completion blob is missing.');
    return read;
  }
}
export function isRelocationCompletionReady(authority: ZLinkAuthoritySnapshot): boolean {
  const completion = authority.relocation?.completion;
  return (
    completion !== undefined &&
    completion.acceptedRequests === completion.completedResults &&
    completion.pendingDeliveries === 0n
  );
}

function completionManifest(bytes: Uint8Array, inventorySha256: string) {
  let manifest: ReturnType<typeof decodeRelocationManifestV1>;
  try {
    manifest = decodeRelocationManifestV1(bytes);
  } catch (error) {
    if (!(error instanceof TypeError || error instanceof RangeError)) throw error;
    return completionDataLost('Completion manifest is corrupt.');
  }
  if (!Buffer.from(manifest.inventoryDigestSha256).equals(Buffer.from(inventorySha256, 'hex')))
    completionDataLost('Completion inventory digest differs.');
  if (
    manifest.chunks.length === 0 ||
    manifest.totalLength === 0n ||
    manifest.totalLength > BigInt(COMPLETION_MAX_BYTES)
  )
    completionDataLost('Invalid completion blob bounds.');
  return manifest;
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

// Command 33 has fixed-width IDs and counters. The variable-width coordinator
// fields use text8, rid and authority-store-version from the service-wire schema.
const TEXT8_BYTES = 255;
const RID_BYTES = 255;
const AUTHORITY_VERSION_BYTES = 4096;
const MAX_U64_TEXT = '18446744073709551615';
const longestDelivery = 'sourceLeaseExpired';

function encodeFailureReply(
  relay: Omit<ServiceMaintenanceReplyRelay, 'payload' | 'terminalResult' | 'failureCode'>
): string {
  return encodeMultipartPayload(
    encodeMaintenanceReplyRelay({
      ...relay,
      terminalResult: RequestResult.InternalError,
      failureCode: ServiceWireFrameworkErrorCode.requestFailed
    })
  ).toString('base64');
}

const maximumFailureReply = encodeFailureReply({
  relocation: { high: 0n, low: 1n },
  targetAttemptGeneration: 1n,
  coordinator: {
    ownerId: 'x'.repeat(TEXT8_BYTES),
    leaseGeneration: 1n,
    nodeRid: 'x'.repeat(RID_BYTES),
    nodeGeneration: 1n,
    expectedAuthorityStoreVersion: 'x'.repeat(AUTHORITY_VERSION_BYTES)
  },
  operation: { high: 0n, low: 1n },
  replyRouteId: 1n,
  participantId: 1n,
  sequence: 1n
});

function failedEntry(entry: RelocationCompletionEntry): RelocationCompletionEntry {
  const { payload: _payload, ...relay } = decodeMaintenanceReplyRelay(
    decodeMultipartParts(Buffer.from(entry.reply, 'base64'), (part) => part)
  );
  return { ...entry, reply: encodeFailureReply(relay) };
}

function completionEntryFinalBytes(entry: RelocationCompletionEntry): number {
  // Canonical key order does not affect size. Base64 requires no JSON escaping.
  return (
    Buffer.byteLength(JSON.stringify({ ...entry, delivery: longestDelivery, reply: '' })) +
    entry.reply.length
  );
}

/** Counts the final canonical bytes without allocating placeholders for the pending requests. */
function completionFits(
  record: RelocationCompletionRecord,
  objectKey: string,
  acceptedRequests: bigint,
  maxBytes: number
): boolean {
  const slot = completionEntryFinalBytes({
    objectKey,
    source: {
      // Each control byte can take six bytes when JSON-escaped.
      ownerId: '\u0001'.repeat(TEXT8_BYTES),
      leaseGeneration: MAX_U64_TEXT,
      nodeRid: '00'.repeat(RID_BYTES),
      nodeGeneration: MAX_U64_TEXT
    },
    operationId: '0'.repeat(32),
    replyRouteId: MAX_U64_TEXT,
    reply: maximumFailureReply,
    delivery: longestDelivery
  });
  const empty = Buffer.byteLength(JSON.stringify({ ...record, entries: [] }));
  const entries = record.entries.reduce(
    (size, value) => size + completionEntryFinalBytes(value),
    0
  );
  const separators = acceptedRequests > 0n ? acceptedRequests - 1n : 0n;
  const remaining = acceptedRequests - BigInt(record.entries.length);
  return BigInt(empty + entries) + remaining * BigInt(slot) + separators <= BigInt(maxBytes);
}

export function requireRelocationCompletionReservation(
  relocationId: string,
  acceptedRequests: ReadonlyMap<string, bigint>
): void {
  const empty: RelocationCompletionRecord = { formatVersion: '1', relocationId, entries: [] };
  for (const [objectKey, accepted] of acceptedRequests) {
    if (accepted > 0n && !completionFits(empty, objectKey, accepted, COMPLETION_MAX_BYTES)) {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.InternalFailure,
        'Accepted relocation requests exceed the completion blob reservation.'
      );
    }
  }
}

export function completionEntryWithinBlobBound(
  record: RelocationCompletionRecord,
  entry: RelocationCompletionEntry,
  maxBytes = COMPLETION_MAX_BYTES,
  acceptedRequests = BigInt(record.entries.length + 1)
): { entry: RelocationCompletionEntry; record: RelocationCompletionRecord; bytes: Buffer } {
  let replacement = { ...record, entries: [...record.entries, entry] };
  if (!completionFits(replacement, entry.objectKey, acceptedRequests, maxBytes)) {
    entry = failedEntry(entry);
    replacement = { ...record, entries: [...record.entries, entry] };
    if (!completionFits(replacement, entry.objectKey, acceptedRequests, maxBytes))
      completionDataLost('Failure reply exceeds the completion blob reservation.');
  }
  return { entry, record: replacement, bytes: encodeRelocationCompletionRecord(replacement) };
}
