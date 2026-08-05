import type {
  ActorRef,
  ZLinkActor,
  ZLinkActorJoinCompletion,
  ZLinkActorJoinOperationId,
  ZLinkBlobReference,
  ZLinkRelocationStore
} from '../../contracts';
import type { ZLinkAuthoritySnapshot } from '../../contracts/Locations';
import type { ZLinkAuthorityStore } from '../locations/internal-store-contracts';
import { ZLinkEncodedPayload, ZLinkMessage } from '../../contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import { crc32c } from '../foundation/service-relocation-runtime';
import { putNewRelocationBlob } from '../locations/relocation-blob';
import { decodeRoutingId, routingIdWireHex } from '../routing-id';
import {
  decodeActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} from './actor-authority-publication';

const RETENTION_MS = 24 * 60 * 60 * 1_000;
const ROOT_VERSION = 2;
const AUTHORITY_VERSION = 1;
const MAX_ROOT_BYTES = 1024 * 1024;
const MAX_RECOVERY_CHUNK_BYTES = 64 * 1024 * 1024;
const MAX_RECOVERY_PAYLOAD_BYTES = 256 * 1024 * 1024;
const RECOVERY_PAYLOAD_INDEX_VERSION = 1;

export type ZLinkDeferredJoinDeliveryCursor =
  | 'prepared'
  | 'committed'
  | 'delivered';

export interface ZLinkDeferredJoinAcceptedRoot {
  readonly authority: ZLinkAuthoritySnapshot;
  readonly reference: ZLinkBlobReference;
  readonly checksumCrc32c: number;
  readonly operationId: ZLinkActorJoinOperationId;
  readonly actor: ActorRef;
  readonly rawReply: Buffer;
  readonly cursor: ZLinkDeferredJoinDeliveryCursor;
  readonly replayCursor: number;
  readonly recovery?: ZLinkDeferredJoinRecoveryManifest;
}

export interface ZLinkDeferredJoinRecoveryManifest {
  readonly targetMeshName: string;
  readonly targetSpotId: string;
  readonly targetSpotGeneration: bigint;
  readonly membershipEpoch: bigint;
  readonly payloadReference: ZLinkBlobReference;
  readonly payloadChecksumCrc32c: number;
  readonly payloadEncodedSize: number;
}

export interface ZLinkDeferredJoinRecoveryInput {
  readonly targetMeshName: string;
  readonly targetSpotId: string;
  readonly targetSpotGeneration: bigint;
  readonly membershipEpoch: bigint;
  readonly request: Uint8Array;
}

interface RecoveryPayloadIndex {
  readonly version: number;
  readonly encodedSize: number;
  readonly checksumCrc32c: number;
  readonly chunks: readonly {
    readonly reference: string;
    readonly encodedSize: number;
    readonly checksumCrc32c: number;
  }[];
}

interface DeferredJoinAuthorityPublication {
  readonly applicationPayload: Buffer;
  readonly reference: ZLinkBlobReference;
  readonly checksumCrc32c: number;
}

/**
 * Keeps a cross-node Accepted Join completion under the Actor authority.
 * Every cursor transition writes and verifies a new immutable root before the
 * authority CAS. A failed callback leaves the committed root available for the
 * next target Actor mailbox retry.
 */
export class ZLinkDeferredJoinAcceptedJournal {
  constructor(
    private readonly authority: ZLinkAuthorityStore,
    private readonly relocation: ZLinkRelocationStore
  ) {}

  async prepare(
    actorId: string,
    operationId: ZLinkActorJoinOperationId,
    actor: ActorRef,
    rawReply: Uint8Array,
    signal?: AbortSignal,
    recovery?: ZLinkDeferredJoinRecoveryInput
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    validateIdentity(actorId, operationId, actor);
    signal?.throwIfAborted();
    const key = encodeAuthorityKey('actor', actorId);
    const read = await this.authority.readAuthority(key, signal);
    if (read.kind !== 'snapshot') {
      throw new Error(`Actor '${actorId}' has no authority for durable Join completion.`);
    }
    requireAuthorityActor(read.payload, actor);
    const existing = await this.readPublished(read, signal);
    if (existing !== undefined) {
      if (isSameOperation(existing, operationId, actor)) {
        return existing;
      }
      if (existing.cursor !== 'delivered') {
        requireSameOperation(existing, operationId, actor);
      }
    }

    const storedRecovery = recovery === undefined
      ? undefined
      : await this.storeRecoveryPayload(recovery, signal);
    let root: Omit<ZLinkDeferredJoinAcceptedRoot, 'authority'>;
    try {
      root = await this.storeRoot({
        operationId,
        actor,
        rawReply: Buffer.from(rawReply),
        cursor: 'prepared',
        replayCursor: 0,
        recovery: storedRecovery
      }, signal);
    } catch (error) {
      if (storedRecovery !== undefined) {
        await this.deleteBestEffort(storedRecovery.payloadReference);
      }
      throw error;
    }
    const currentPublication = decodeAuthorityPublication(read.payload);
    const publication: DeferredJoinAuthorityPublication = {
      applicationPayload: Buffer.from(
        currentPublication?.applicationPayload ?? read.payload
      ),
      reference: root.reference,
      checksumCrc32c: root.checksumCrc32c
    };
    const result = await this.authority.compareExchangeAuthority(
      key,
      read.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: encodeAuthorityPublication(publication)
      },
      signal
    );
    if (result.kind !== 'stored') {
      await this.deleteBestEffort(root.reference);
      if (storedRecovery !== undefined) {
        await this.deleteBestEffort(storedRecovery.payloadReference);
      }
      const current = result.kind === 'conflict' && result.current.kind === 'snapshot'
        ? await this.readPublished(result.current, signal)
        : undefined;
      if (current !== undefined) {
        requireSameOperation(current, operationId, actor);
        return current;
      }
      throw new Error(`Actor '${actorId}' authority rejected Join completion publication.`);
    }
    if (existing !== undefined) {
      await this.deleteBestEffort(existing.reference);
      await this.deleteRecoveryBestEffort(existing.recovery);
    }
    return {
      ...root,
      authority: storedSnapshot(result)
    };
  }

  async recover(
    actorId: string,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot | undefined> {
    const read = await this.authority.readAuthority(
      encodeAuthorityKey('actor', actorId),
      signal
    );
    return read.kind === 'snapshot'
      ? await this.readPublished(read, signal)
      : undefined;
  }

  markCommitted(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor?: ActorRef,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    if (actor !== undefined) requireSameActor(root, actor);
    return this.moveCursor(root, 'committed', actor, signal);
  }

  markDelivered(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    return this.moveCursor(root, 'delivered', undefined, signal);
  }

  async markRecoveryMessageReplayed(
    root: ZLinkDeferredJoinAcceptedRoot,
    nextReplayCursor: number,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    if (
      !Number.isSafeInteger(nextReplayCursor)
      || nextReplayCursor !== root.replayCursor + 1
    ) {
      throw new Error('Deferred Join recovery replay cursor transition is invalid.');
    }
    const key = encodeAuthorityKey('actor', root.actor.actorId);
    const read = await this.authority.readAuthority(key, signal);
    if (read.kind !== 'snapshot') {
      throw new Error(`Actor '${root.actor.actorId}' authority disappeared during queue replay.`);
    }
    const current = await this.readPublished(read, signal);
    if (current === undefined) {
      throw new Error(`Actor '${root.actor.actorId}' no longer references its Join recovery root.`);
    }
    requireSameOperation(current, root.operationId, root.actor);
    if (current.replayCursor >= nextReplayCursor) return current;
    if (current.replayCursor + 1 !== nextReplayCursor) {
      throw new Error('Deferred Join recovery replay cursor has a gap.');
    }
    const replacement = await this.storeRoot({
      operationId: current.operationId,
      actor: current.actor,
      rawReply: current.rawReply,
      cursor: current.cursor,
      replayCursor: nextReplayCursor,
      recovery: current.recovery
    }, signal);
    const publication = decodeAuthorityPublication(read.payload);
    if (publication === undefined) {
      await this.deleteBestEffort(replacement.reference);
      throw new Error('Actor authority lost its deferred Join publication.');
    }
    const result = await this.authority.compareExchangeAuthority(
      key,
      read.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: encodeAuthorityPublication({
          applicationPayload: publication.applicationPayload,
          reference: replacement.reference,
          checksumCrc32c: replacement.checksumCrc32c
        })
      },
      signal
    );
    if (result.kind !== 'stored') {
      await this.deleteBestEffort(replacement.reference);
      const recovered = await this.recover(root.actor.actorId, signal);
      if (recovered === undefined) {
        throw new Error('Deferred Join replay CAS conflicted without a recoverable root.');
      }
      requireSameOperation(recovered, root.operationId, root.actor);
      return recovered;
    }
    await this.deleteBestEffort(current.reference);
    return {
      ...replacement,
      authority: storedSnapshot(result)
    };
  }

  async readRecoveryPayload(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<Buffer> {
    const recovery = root.recovery;
    if (recovery === undefined) {
      throw new Error(`Actor '${root.actor.actorId}' has no deferred Join recovery payload.`);
    }
    const indexRead = await this.relocation.read(recovery.payloadReference, signal);
    if (indexRead.kind !== 'found') {
      throw new Error(
        `Actor '${root.actor.actorId}' deferred Join recovery payload is missing or corrupt.`
      );
    }
    const index = decodeRecoveryPayloadIndex(indexRead.bytes);
    if (
      index.encodedSize !== recovery.payloadEncodedSize
      || index.checksumCrc32c !== recovery.payloadChecksumCrc32c
    ) {
      throw new Error(
        `Actor '${root.actor.actorId}' deferred Join recovery payload is missing or corrupt.`
      );
    }
    const chunks = await Promise.all(index.chunks.map(async chunk => {
      const read = await this.relocation.read(
        { value: chunk.reference } as ZLinkBlobReference,
        signal
      );
      if (
        read.kind !== 'found'
        || read.bytes.byteLength !== chunk.encodedSize
        || crc32c(read.bytes) !== chunk.checksumCrc32c
      ) {
        throw new Error(
          `Actor '${root.actor.actorId}' deferred Join recovery chunk is missing or corrupt.`
        );
      }
      return Buffer.from(read.bytes);
    }));
    const payload = Buffer.concat(chunks, index.encodedSize);
    if (payload.byteLength !== index.encodedSize || crc32c(payload) !== index.checksumCrc32c) {
      throw new Error(
        `Actor '${root.actor.actorId}' deferred Join recovery payload is missing or corrupt.`
      );
    }
    return payload;
  }

  async discardPrepared(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    const key = encodeAuthorityKey('actor', root.actor.actorId);
    for (let attempt = 0; attempt < 3; attempt++) {
      const read = await this.authority.readAuthority(key, signal);
      if (read.kind !== 'snapshot') return;
      const current = await this.readPublished(read, signal);
      if (current === undefined) return;
      requireSameOperation(current, root.operationId, root.actor);
      if (current.cursor !== 'prepared') return;
      const publication = decodeAuthorityPublication(read.payload);
      if (publication === undefined) return;
      const result = await this.authority.compareExchangeAuthority(
        key,
        read.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: publication.applicationPayload
        },
        signal
      );
      if (result.kind === 'stored') {
        await this.deleteBestEffort(current.reference);
        await this.deleteRecoveryBestEffort(current.recovery);
        return;
      }
    }
    throw new Error(`Actor '${root.actor.actorId}' deferred Join preparation could not be discarded.`);
  }

  async deliver(
    root: ZLinkDeferredJoinAcceptedRoot,
    actor: ZLinkActor,
    actorRef: ActorRef,
    submitMailbox: <T>(operation: () => Promise<T>) => Promise<T>,
    signal?: AbortSignal,
    retainRecoveryRoot = false
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    requireSameActor(root, actorRef);
    return await submitMailbox(async () => {
      const current = await this.recover(actorRef.actorId, signal);
      if (current === undefined) {
        if (root.cursor === 'delivered') return root;
        throw new Error(`Actor '${actorRef.actorId}' lost its deferred Join completion root.`);
      }
      requireSameOperation(current, root.operationId, actorRef);
      if (current.cursor === 'delivered') {
        if (!retainRecoveryRoot) {
          await this.releaseDelivered(current, signal);
        }
        return current;
      }
      if (current.cursor !== 'committed') {
        throw new Error(`Actor '${actorRef.actorId}' Join completion is not committed.`);
      }
      const completion: ZLinkActorJoinCompletion = {
        status: 'accepted',
        operationId: current.operationId,
        actor: current.actor,
        reply: current.rawReply.byteLength === 0
          ? undefined
          : ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(current.rawReply))
      };
      await actor.onJoinCompleted?.(completion);
      let delivered = current;
      for (let attempt = 0; attempt < 3; attempt++) {
        delivered = await this.markDelivered(delivered, signal);
        if (delivered.cursor === 'delivered') {
          if (!retainRecoveryRoot) {
            await this.releaseDelivered(delivered, signal);
          }
          return delivered;
        }
      }
      throw new Error(
        `Actor '${actorRef.actorId}' Join completion callback ran but its Delivered cursor could not be stored.`
      );
    });
  }

  releaseRecovery(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    if (root.cursor !== 'delivered') {
      throw new Error(
        `Actor '${root.actor.actorId}' recovery root is not Delivered.`
      );
    }
    return this.releaseDelivered(root, signal);
  }

  private async releaseDelivered(
    root: ZLinkDeferredJoinAcceptedRoot,
    signal?: AbortSignal
  ): Promise<void> {
    const key = encodeAuthorityKey('actor', root.actor.actorId);
    for (let attempt = 0; attempt < 3; attempt++) {
      const read = await this.authority.readAuthority(key, signal);
      if (read.kind !== 'snapshot') return;
      const current = await this.readPublished(read, signal);
      if (current === undefined) return;
      requireSameOperation(current, root.operationId, root.actor);
      if (current.cursor !== 'delivered') {
        throw new Error(
          `Actor '${root.actor.actorId}' Join completion root is not Delivered.`
        );
      }
      const publication = decodeAuthorityPublication(read.payload);
      if (publication === undefined) return;
      const result = await this.authority.compareExchangeAuthority(
        key,
        read.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: publication.applicationPayload
        },
        signal
      );
      if (result.kind === 'stored') {
        await this.deleteBestEffort(current.reference);
        await this.deleteRecoveryBestEffort(current.recovery);
        return;
      }
    }
    throw new Error(
      `Actor '${root.actor.actorId}' Delivered Join completion could not be released.`
    );
  }

  private async moveCursor(
    root: ZLinkDeferredJoinAcceptedRoot,
    next: ZLinkDeferredJoinDeliveryCursor,
    actor: ActorRef | undefined,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    const currentIndex = cursorIndex(root.cursor);
    const nextIndex = cursorIndex(next);
    if (nextIndex < currentIndex || nextIndex > currentIndex + 1) {
      throw new Error('Deferred Join completion cursor transition is invalid.');
    }
    if (next === root.cursor) return root;

    const key = encodeAuthorityKey('actor', root.actor.actorId);
    const read = await this.authority.readAuthority(key, signal);
    if (read.kind !== 'snapshot') {
      throw new Error(`Actor '${root.actor.actorId}' authority disappeared during Join completion.`);
    }
    const current = await this.readPublished(read, signal);
    if (current === undefined) {
      throw new Error(`Actor '${root.actor.actorId}' no longer references its Join completion root.`);
    }
    requireSameOperation(current, root.operationId, root.actor);
    if (cursorIndex(current.cursor) >= nextIndex) return current;

    const replacement = await this.storeRoot({
      operationId: current.operationId,
      actor: actor ?? current.actor,
      rawReply: current.rawReply,
      cursor: next,
      replayCursor: current.replayCursor,
      recovery: current.recovery
    }, signal);
    const publication = decodeAuthorityPublication(read.payload);
    if (publication === undefined) {
      await this.deleteBestEffort(replacement.reference);
      throw new Error('Actor authority lost its deferred Join publication.');
    }
    const identity = decodeActorAuthorityIdentity(publication.applicationPayload);
    if (identity === undefined) {
      await this.deleteBestEffort(replacement.reference);
      throw new Error('Actor authority identity is invalid.');
    }
    const result = await this.authority.compareExchangeAuthority(
      key,
      read.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        payload: encodeAuthorityPublication({
          applicationPayload: actor === undefined
            ? publication.applicationPayload
            : encodeActorAuthorityIdentity({ ...identity, actor }),
          reference: replacement.reference,
          checksumCrc32c: replacement.checksumCrc32c
        })
      },
      signal
    );
    if (result.kind !== 'stored') {
      await this.deleteBestEffort(replacement.reference);
      const recovered = await this.recover(root.actor.actorId, signal);
      if (recovered === undefined) {
        throw new Error('Deferred Join cursor CAS conflicted without a recoverable root.');
      }
      requireSameOperation(recovered, root.operationId, root.actor);
      return recovered;
    }
    await this.deleteBestEffort(current.reference);
    return {
      ...replacement,
      authority: storedSnapshot(result)
    };
  }

  private async readPublished(
    authority: ZLinkAuthoritySnapshot,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinAcceptedRoot | undefined> {
    const publication = decodeAuthorityPublication(authority.payload);
    if (publication === undefined) return undefined;
    const read = await this.relocation.read(publication.reference, signal);
    if (
      read.kind !== 'found'
      || crc32c(read.bytes) !== publication.checksumCrc32c
    ) {
      throw new Error('Published deferred Join completion root is missing or corrupt.');
    }
    return {
      authority,
      reference: publication.reference,
      checksumCrc32c: publication.checksumCrc32c,
      ...decodeRoot(read.bytes)
    };
  }

  private async storeRoot(
    value: Omit<
      ZLinkDeferredJoinAcceptedRoot,
      'authority' | 'reference' | 'checksumCrc32c'
    >,
    signal?: AbortSignal
  ): Promise<Omit<ZLinkDeferredJoinAcceptedRoot, 'authority'>> {
    const payload = encodeRoot(value);
    const checksumCrc32c = crc32c(payload);
    const stored = await putNewRelocationBlob(
      this.relocation,
      payload,
      RETENTION_MS,
      signal
    );
    if (
      stored.expiresAt.getTime() <= stored.storeNow.getTime()
    ) {
      await this.deleteBestEffort(stored.reference);
      throw new Error('Relocation Store returned an invalid deferred Join root receipt.');
    }
    const read = await this.relocation.read(stored.reference, signal);
    if (
      read.kind !== 'found'
      || !Buffer.from(read.bytes).equals(payload)
    ) {
      await this.deleteBestEffort(stored.reference);
      throw new Error('Relocation Store failed deferred Join root verification.');
    }
    return {
      ...value,
      rawReply: Buffer.from(value.rawReply),
      recovery: copyRecovery(value.recovery),
      reference: stored.reference,
      checksumCrc32c
    };
  }

  private async storeRecoveryPayload(
    value: ZLinkDeferredJoinRecoveryInput,
    signal?: AbortSignal
  ): Promise<ZLinkDeferredJoinRecoveryManifest> {
    const request = Buffer.from(value.request);
    if (
      request.byteLength === 0
      || request.byteLength > MAX_RECOVERY_PAYLOAD_BYTES
      || value.targetMeshName.length === 0
      || value.targetSpotId.length === 0
      || value.targetSpotGeneration <= 0n
      || value.membershipEpoch <= 0n
    ) {
      throw new Error('Deferred Join recovery payload is invalid.');
    }
    const checksum = crc32c(request);
    const chunks: Array<RecoveryPayloadIndex['chunks'][number]> = [];
    try {
      for (let offset = 0; offset < request.byteLength; offset += MAX_RECOVERY_CHUNK_BYTES) {
        const bytes = request.subarray(
          offset,
          Math.min(offset + MAX_RECOVERY_CHUNK_BYTES, request.byteLength)
        );
        const stored = await putNewRelocationBlob(
          this.relocation,
          bytes,
          RETENTION_MS,
          signal
        );
        const chunkChecksum = crc32c(bytes);
        const read = await this.relocation.read(stored.reference, signal);
        if (
          read.kind !== 'found'
          || read.bytes.byteLength !== bytes.byteLength
          || crc32c(read.bytes) !== chunkChecksum
        ) {
          await this.deleteBestEffort(stored.reference);
          throw new Error(
            'Relocation Store failed deferred Join recovery chunk verification.'
          );
        }
        chunks.push({
          reference: stored.reference.value,
          encodedSize: bytes.byteLength,
          checksumCrc32c: chunkChecksum
        });
      }
      const index = encodeRecoveryPayloadIndex({
        version: RECOVERY_PAYLOAD_INDEX_VERSION,
        encodedSize: request.byteLength,
        checksumCrc32c: checksum,
        chunks
      });
      const stored = await putNewRelocationBlob(
        this.relocation,
        index,
        RETENTION_MS,
        signal
      );
      return {
        targetMeshName: value.targetMeshName,
        targetSpotId: value.targetSpotId,
        targetSpotGeneration: value.targetSpotGeneration,
        membershipEpoch: value.membershipEpoch,
        payloadReference: stored.reference,
        payloadChecksumCrc32c: checksum,
        payloadEncodedSize: request.byteLength
      };
    } catch (error) {
      await Promise.all(chunks.map(chunk =>
        this.deleteBestEffort({ value: chunk.reference } as ZLinkBlobReference)
      ));
      throw error;
    }
  }

  private async deleteRecoveryBestEffort(
    recovery: ZLinkDeferredJoinRecoveryManifest | undefined
  ): Promise<void> {
    if (recovery !== undefined) {
      try {
        const read = await this.relocation.read(recovery.payloadReference);
        if (read.kind === 'found') {
          const index = decodeRecoveryPayloadIndex(read.bytes);
          await Promise.all(index.chunks.map(chunk =>
            this.deleteBestEffort({ value: chunk.reference } as ZLinkBlobReference)
          ));
        }
      } catch {
        // The root retention remains the final cleanup boundary for unreadable indexes.
      }
      await this.deleteBestEffort(recovery.payloadReference);
    }
  }

  private async deleteBestEffort(reference: ZLinkBlobReference): Promise<void> {
    try {
      await this.relocation.delete(reference);
    } catch {
      // Fixed retention remains the orphan cleanup boundary.
    }
  }
}

function requireAuthorityActor(payload: Uint8Array, actor: ActorRef): void {
  const publication = decodeAuthorityPublication(payload);
  const identity = decodeActorAuthorityIdentity(
    publication?.applicationPayload ?? payload
  );
  if (
    identity === undefined
    || identity.actor.actorId !== actor.actorId
    || identity.actor.objectGeneration !== actor.objectGeneration
    || String(identity.actor.nodeRid) !== String(actor.nodeRid)
  ) {
    throw new Error(`Actor '${actor.actorId}' authority fence does not match its ActorRef.`);
  }
}

function encodeRoot(
  value: Omit<
    ZLinkDeferredJoinAcceptedRoot,
    'authority' | 'reference' | 'checksumCrc32c'
  >
): Buffer {
  const nodeRidHex = routingIdWireHex(value.actor.nodeRid);
  const encoded = Buffer.from(JSON.stringify({
    version: ROOT_VERSION,
    operationHigh: value.operationId.high.toString(),
    operationLow: value.operationId.low.toString(),
    actorId: value.actor.actorId,
    actorGeneration: value.actor.objectGeneration.toString(),
    actorMeshName: value.actor.meshName,
    actorNodeRid: String(value.actor.nodeRid),
    actorNodeRidHex: nodeRidHex,
    rawReply: Buffer.from(value.rawReply).toString('base64'),
    cursor: value.cursor,
    replayCursor: value.replayCursor,
    recovery: value.recovery === undefined
      ? undefined
      : {
          targetMeshName: value.recovery.targetMeshName,
          targetSpotId: value.recovery.targetSpotId,
          targetSpotGeneration: value.recovery.targetSpotGeneration.toString(),
          membershipEpoch: value.recovery.membershipEpoch.toString(),
          payloadReference: value.recovery.payloadReference.value,
          payloadChecksumCrc32c: value.recovery.payloadChecksumCrc32c,
          payloadEncodedSize: value.recovery.payloadEncodedSize
        }
  }), 'utf8');
  if (encoded.byteLength > MAX_ROOT_BYTES) {
    throw new Error('Deferred Join completion root exceeds 1 MiB.');
  }
  return encoded;
}

function decodeRoot(
  payload: Uint8Array
): Omit<ZLinkDeferredJoinAcceptedRoot, 'authority' | 'reference' | 'checksumCrc32c'> {
  if (payload.byteLength === 0 || payload.byteLength > MAX_ROOT_BYTES) {
    throw new Error('Deferred Join completion root size is invalid.');
  }
  const value = JSON.parse(Buffer.from(payload).toString('utf8')) as Record<string, unknown>;
  if (
    value.version !== ROOT_VERSION
    || typeof value.operationHigh !== 'string'
    || typeof value.operationLow !== 'string'
    || typeof value.actorId !== 'string'
    || typeof value.actorGeneration !== 'string'
    || typeof value.actorMeshName !== 'string'
    || typeof value.actorNodeRid !== 'string'
    || typeof value.rawReply !== 'string'
    || (
      value.replayCursor !== undefined
      && (!Number.isSafeInteger(value.replayCursor) || (value.replayCursor as number) < 0)
    )
    || !isCursor(value.cursor)
  ) {
    throw new Error('Deferred Join completion root is invalid.');
  }
  const rawReply = Buffer.from(value.rawReply, 'base64');
  if (rawReply.toString('base64') !== value.rawReply) {
    throw new Error('Deferred Join completion reply is not canonical base64.');
  }
  const actorGeneration = BigInt(value.actorGeneration);
  const operationId = {
    high: BigInt(value.operationHigh),
    low: BigInt(value.operationLow)
  };
  const actor: ActorRef = {
    actorId: value.actorId,
    objectGeneration: actorGeneration,
    meshName: value.actorMeshName,
    nodeRid: decodeRoutingId(
      value.actorNodeRid,
      typeof value.actorNodeRidHex === 'string' ? value.actorNodeRidHex : undefined
    )
  };
  validateIdentity(value.actorId, operationId, actor);
  const recovery = decodeRecovery(value.recovery);
  return {
    operationId,
    actor,
    rawReply,
    cursor: value.cursor,
    replayCursor: value.replayCursor === undefined ? 0 : value.replayCursor as number,
    recovery
  };
}

function decodeRecovery(value: unknown): ZLinkDeferredJoinRecoveryManifest | undefined {
  if (value === undefined) return undefined;
  if (
    typeof value !== 'object'
    || value === null
    || typeof (value as { targetMeshName?: unknown }).targetMeshName !== 'string'
    || typeof (value as { targetSpotId?: unknown }).targetSpotId !== 'string'
    || typeof (value as { targetSpotGeneration?: unknown }).targetSpotGeneration !== 'string'
    || typeof (value as { membershipEpoch?: unknown }).membershipEpoch !== 'string'
    || typeof (value as { payloadReference?: unknown }).payloadReference !== 'string'
    || !validCrc32c(
      (value as { payloadChecksumCrc32c?: unknown }).payloadChecksumCrc32c
    )
    || !Number.isSafeInteger(
      (value as { payloadEncodedSize?: unknown }).payloadEncodedSize
    )
  ) {
    throw new Error('Deferred Join recovery manifest is invalid.');
  }
  const targetSpotGeneration = BigInt(
    (value as { targetSpotGeneration: string }).targetSpotGeneration
  );
  const membershipEpoch = BigInt(
    (value as { membershipEpoch: string }).membershipEpoch
  );
  if (
    (value as { targetMeshName: string }).targetMeshName.length === 0
    || (value as { targetSpotId: string }).targetSpotId.length === 0
    || targetSpotGeneration <= 0n
    || membershipEpoch <= 0n
    || (value as { payloadReference: string }).payloadReference.length === 0
    || (value as { payloadEncodedSize: number }).payloadEncodedSize < 1
    || (value as { payloadEncodedSize: number }).payloadEncodedSize
      > MAX_RECOVERY_PAYLOAD_BYTES
  ) {
    throw new Error('Deferred Join recovery manifest is invalid.');
  }
  return {
    targetMeshName: (value as { targetMeshName: string }).targetMeshName,
    targetSpotId: (value as { targetSpotId: string }).targetSpotId,
    targetSpotGeneration,
    membershipEpoch,
    payloadReference: {
      value: (value as { payloadReference: string }).payloadReference
    } as ZLinkBlobReference,
    payloadChecksumCrc32c:
      (value as { payloadChecksumCrc32c: number }).payloadChecksumCrc32c,
    payloadEncodedSize: (value as { payloadEncodedSize: number }).payloadEncodedSize
  };
}

function copyRecovery(
  value: ZLinkDeferredJoinRecoveryManifest | undefined
): ZLinkDeferredJoinRecoveryManifest | undefined {
  return value === undefined
    ? undefined
    : {
        ...value,
        payloadReference: { value: value.payloadReference.value } as ZLinkBlobReference
      };
}

function encodeRecoveryPayloadIndex(value: RecoveryPayloadIndex): Buffer {
  return Buffer.from(JSON.stringify(value), 'utf8');
}

function decodeRecoveryPayloadIndex(bytes: Uint8Array): RecoveryPayloadIndex {
  let value: unknown;
  try {
    value = JSON.parse(Buffer.from(bytes).toString('utf8'));
  } catch {
    throw new Error('Deferred Join recovery payload index is invalid.');
  }
  if (
    typeof value !== 'object'
    || value === null
    || (value as { version?: unknown }).version !== RECOVERY_PAYLOAD_INDEX_VERSION
    || !Number.isSafeInteger((value as { encodedSize?: unknown }).encodedSize)
    || (value as { encodedSize: number }).encodedSize < 1
    || (value as { encodedSize: number }).encodedSize > MAX_RECOVERY_PAYLOAD_BYTES
    || !validCrc32c((value as { checksumCrc32c?: unknown }).checksumCrc32c)
    || !Array.isArray((value as { chunks?: unknown }).chunks)
  ) {
    throw new Error('Deferred Join recovery payload index is invalid.');
  }
  const chunks = (value as { chunks: unknown[] }).chunks;
  if (chunks.length < 1 || chunks.length > 4) {
    throw new Error('Deferred Join recovery payload index is invalid.');
  }
  let total = 0;
  const decoded = chunks.map(chunk => {
    if (
      typeof chunk !== 'object'
      || chunk === null
      || typeof (chunk as { reference?: unknown }).reference !== 'string'
      || Buffer.byteLength((chunk as { reference: string }).reference, 'utf8') < 1
      || Buffer.byteLength((chunk as { reference: string }).reference, 'utf8') > 4096
      || !Number.isSafeInteger((chunk as { encodedSize?: unknown }).encodedSize)
      || (chunk as { encodedSize: number }).encodedSize < 1
      || (chunk as { encodedSize: number }).encodedSize > MAX_RECOVERY_CHUNK_BYTES
      || !validCrc32c((chunk as { checksumCrc32c?: unknown }).checksumCrc32c)
    ) {
      throw new Error('Deferred Join recovery payload index is invalid.');
    }
    total += (chunk as { encodedSize: number }).encodedSize;
    return {
      reference: (chunk as { reference: string }).reference,
      encodedSize: (chunk as { encodedSize: number }).encodedSize,
      checksumCrc32c: (chunk as { checksumCrc32c: number }).checksumCrc32c
    };
  });
  if (total !== (value as { encodedSize: number }).encodedSize) {
    throw new Error('Deferred Join recovery payload index is invalid.');
  }
  return {
    version: RECOVERY_PAYLOAD_INDEX_VERSION,
    encodedSize: (value as { encodedSize: number }).encodedSize,
    checksumCrc32c: (value as { checksumCrc32c: number }).checksumCrc32c,
    chunks: decoded
  };
}

function validCrc32c(value: unknown): value is number {
  return Number.isSafeInteger(value) && (value as number) >= 0
    && (value as number) <= 0xffff_ffff;
}

function encodeAuthorityPublication(value: DeferredJoinAuthorityPublication): Buffer {
  return Buffer.from(JSON.stringify({
    version: AUTHORITY_VERSION,
    applicationPayload: value.applicationPayload.toString('base64'),
    deferredJoin: {
      reference: value.reference.value,
      checksumCrc32c: value.checksumCrc32c
    }
  }), 'utf8');
}

function decodeAuthorityPublication(
  payload: Uint8Array
): DeferredJoinAuthorityPublication | undefined {
  let value: {
    readonly version?: unknown;
    readonly applicationPayload?: unknown;
    readonly deferredJoin?: {
      readonly reference?: unknown;
      readonly checksumCrc32c?: unknown;
    };
  };
  try {
    value = JSON.parse(Buffer.from(payload).toString('utf8'));
  } catch {
    return undefined;
  }
  if (
    value.version !== AUTHORITY_VERSION
    || typeof value.applicationPayload !== 'string'
    || typeof value.deferredJoin?.reference !== 'string'
    || typeof value.deferredJoin.checksumCrc32c !== 'number'
  ) {
    return undefined;
  }
  const applicationPayload = Buffer.from(value.applicationPayload, 'base64');
  if (applicationPayload.toString('base64') !== value.applicationPayload) {
    throw new Error('Deferred Join authority application payload is invalid.');
  }
  return {
    applicationPayload,
    reference: { value: value.deferredJoin.reference } as ZLinkBlobReference,
    checksumCrc32c: value.deferredJoin.checksumCrc32c
  };
}

function validateIdentity(
  actorId: string,
  operationId: ZLinkActorJoinOperationId,
  actor: ActorRef
): void {
  if (
    actorId.length === 0
    || actor.actorId !== actorId
    || actor.objectGeneration <= 0n
    || operationId.high === 0n && operationId.low === 0n
  ) {
    throw new Error('Deferred Join completion identity is invalid.');
  }
}

function requireSameOperation(
  root: ZLinkDeferredJoinAcceptedRoot,
  operationId: ZLinkActorJoinOperationId,
  actor: ActorRef
): void {
  if (!isSameOperation(root, operationId, actor)) {
    throw new Error(`Actor '${actor.actorId}' already has a different durable Join completion.`);
  }
}

function isSameOperation(
  root: ZLinkDeferredJoinAcceptedRoot,
  operationId: ZLinkActorJoinOperationId,
  actor: ActorRef
): boolean {
  return root.operationId.high === operationId.high
    && root.operationId.low === operationId.low
    && root.actor.actorId === actor.actorId
    && root.actor.objectGeneration === actor.objectGeneration;
}

function requireSameActor(
  root: ZLinkDeferredJoinAcceptedRoot,
  actor: ActorRef
): void {
  if (
    root.actor.actorId !== actor.actorId
    || root.actor.objectGeneration !== actor.objectGeneration
  ) {
    throw new Error(
      'Deferred Join completion generation fence is stale '
      + `(root ${root.actor.actorId}/${root.actor.objectGeneration}, `
      + `actor ${actor.actorId}/${actor.objectGeneration}).`
    );
  }
}

function cursorIndex(cursor: ZLinkDeferredJoinDeliveryCursor): number {
  return cursor === 'prepared' ? 1 : cursor === 'committed' ? 2 : 3;
}

function isCursor(value: unknown): value is ZLinkDeferredJoinDeliveryCursor {
  return value === 'prepared' || value === 'committed' || value === 'delivered';
}

function storedSnapshot(
  result: Extract<
    Awaited<ReturnType<ZLinkAuthorityStore['compareExchangeAuthority']>>,
    { readonly kind: 'stored' }
  >
): ZLinkAuthoritySnapshot {
  const { kind: _kind, ...snapshot } = result;
  return { kind: 'snapshot', ...snapshot };
}
