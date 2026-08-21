import { createHash } from 'node:crypto';
import type {
  ActorRef,
  ZLinkActor,
  ZLinkActorJoinCompletion,
  ZLinkActorJoinOperationId,
  ZLinkBlobReference,
  ZLinkRelocationStore
} from '../../contracts';
import type { ZLinkAuthoritySnapshot } from '../locations/internal-location-contracts';
import type { ZLinkAuthorityStore } from '../locations/internal-store-contracts';
import { ZLinkEncodedPayload, ZLinkMessage } from '../../contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  crc32c,
  ServiceRelocationAuthorityPayloadCodec,
  replaceServiceRelocationAuthorityApplicationPayload,
  serviceRelocationAuthorityApplicationPayload,
  type ServiceRelocationPublication
} from '../foundation/service-relocation-runtime';
import { putNewRelocationBlob } from '../locations/relocation-blob';
import { decodeRoutingId, routingIdWireHex } from '../routing-id';
import {
  decodeRelocatingActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} from './actor-authority-publication';
import {
  replaceActorRelocationAuthorityApplicationPayload
} from './actor-authority-payload-codec';

const RETENTION_MS = 24 * 60 * 60 * 1_000;
const ROOT_VERSION = 2;
const MAX_ROOT_BYTES = 1024 * 1024;
const JOURNAL_INVENTORY_DOMAIN = Buffer.from(
  'zlink-node-deferred-join-authority-v1\0',
  'utf8'
);

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
}

interface DeferredJoinAuthorityPublication {
  readonly applicationPayload: Buffer;
  readonly reference: ZLinkBlobReference;
  readonly checksumCrc32c: number;
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
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
    signal?: AbortSignal
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

    const root = await this.storeRoot({
      operationId,
      actor,
      rawReply: Buffer.from(rawReply),
      cursor: 'prepared'
    }, signal);
    const currentPublication = decodeAuthorityPublication(read.payload);
    const publication: DeferredJoinAuthorityPublication = {
      applicationPayload: Buffer.from(
        currentPublication?.applicationPayload
          ?? serviceRelocationAuthorityApplicationPayload(read.payload)
      ),
      reference: root.reference,
      checksumCrc32c: root.checksumCrc32c,
      aggregateId: operationAggregateId(operationId),
      aggregateGeneration: actor.objectGeneration,
      targetOwnerId: read.ownerId,
      targetOwnerLeaseGeneration: read.ownerLeaseGeneration
    };
    const result = await this.authority.compareExchangeAuthority(
      key,
      read.storeVersion,
      {
        kind: 'put',
        generationTransition: 'preserve',
        //  Preserve the outer relocation metadata: only the inner
        //  application payload belongs to this journal.
        payload: replaceDeferredJoinAuthorityPublication(
          read.payload,
          encodeAuthorityPublication(publication)
        )
      },
      signal
    );
    if (result.kind !== 'stored') {
      await this.deleteBestEffort(root.reference);
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
          payload: replaceDeferredJoinAuthorityPublication(
            read.payload,
            publication.applicationPayload
          )
        },
        signal
      );
      if (result.kind === 'stored') {
        await this.deleteBestEffort(current.reference);
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
    signal?: AbortSignal
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
        await this.releaseDelivered(current, signal);
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
          await this.releaseDelivered(delivered, signal);
          return delivered;
        }
      }
      throw new Error(
        `Actor '${actorRef.actorId}' Join completion callback ran but its Delivered cursor could not be stored.`
      );
    });
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
          payload: replaceDeferredJoinAuthorityPublication(
            read.payload,
            publication.applicationPayload
          )
        },
        signal
      );
      if (result.kind === 'stored') {
        await this.deleteBestEffort(current.reference);
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
      cursor: next
    }, signal);
    const publication = decodeAuthorityPublication(read.payload);
    if (publication === undefined) {
      await this.deleteBestEffort(replacement.reference);
      throw new Error('Actor authority lost its deferred Join publication.');
    }
    const identity = decodeRelocatingActorAuthorityIdentity(
      publication.applicationPayload,
      read.objectGeneration
    );
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
        payload: replaceDeferredJoinAuthorityPublication(
          read.payload,
          encodeAuthorityPublication({
            ...publication,
            applicationPayload: actor === undefined
              ? publication.applicationPayload
              : replaceActorRelocationAuthorityApplicationPayload(
                  publication.applicationPayload,
                  encodeActorAuthorityIdentity({ ...identity, actor })
                ),
            reference: replacement.reference,
            checksumCrc32c: replacement.checksumCrc32c
          })
        )
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
      reference: stored.reference,
      checksumCrc32c
    };
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
  const identity = decodeRelocatingActorAuthorityIdentity(
    publication?.applicationPayload
      ?? serviceRelocationAuthorityApplicationPayload(payload),
    actor.objectGeneration
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
    cursor: value.cursor
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
  return {
    operationId,
    actor,
    rawReply,
    cursor: value.cursor
  };
}

function encodeAuthorityPublication(value: DeferredJoinAuthorityPublication): Buffer {
  return Buffer.from(new ServiceRelocationAuthorityPayloadCodec().publish(
    value.applicationPayload,
    {
      reference: value.reference.value,
      checksumCrc32c: value.checksumCrc32c,
      aggregateId: value.aggregateId,
      aggregateGeneration: value.aggregateGeneration,
      inventoryDigest: journalInventoryDigest(value.applicationPayload),
      targetOwnerId: value.targetOwnerId,
      targetOwnerLeaseGeneration: value.targetOwnerLeaseGeneration
    }
  ));
}

function decodeAuthorityPublication(
  payload: Uint8Array
): DeferredJoinAuthorityPublication | undefined {
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  const candidates = Buffer.from(outerApplication).equals(Buffer.from(payload))
    ? [Buffer.from(payload)]
    : [Buffer.from(outerApplication), Buffer.from(payload)];
  for (const candidate of candidates) {
    const publication = decodeDirectAuthorityPublication(candidate);
    if (publication !== undefined) return publication;
  }
  return undefined;
}

function replaceDeferredJoinAuthorityPublication(
  payload: Uint8Array,
  replacement: Uint8Array
): Buffer {
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  if (
    !Buffer.from(outerApplication).equals(Buffer.from(payload))
    && decodeDirectAuthorityPublication(outerApplication) !== undefined
  ) {
    return replaceServiceRelocationAuthorityApplicationPayload(payload, replacement);
  }
  return decodeDirectAuthorityPublication(payload) === undefined
    ? replaceServiceRelocationAuthorityApplicationPayload(payload, replacement)
    : Buffer.from(replacement);
}

function decodeDirectAuthorityPublication(
  payload: Uint8Array
): DeferredJoinAuthorityPublication | undefined {
  const publication = new ServiceRelocationAuthorityPayloadCodec().read(payload);
  if (publication === undefined) return undefined;
  const applicationPayload = serviceRelocationAuthorityApplicationPayload(payload);
  return publication.inventoryDigest === journalInventoryDigest(applicationPayload)
    ? deferredJoinPublication(applicationPayload, publication)
    : undefined;
}

function deferredJoinPublication(
  applicationPayload: Buffer,
  publication: ServiceRelocationPublication
): DeferredJoinAuthorityPublication {
  return {
    applicationPayload,
    reference: { value: publication.reference } as ZLinkBlobReference,
    checksumCrc32c: publication.checksumCrc32c,
    aggregateId: publication.aggregateId,
    aggregateGeneration: publication.aggregateGeneration,
    targetOwnerId: publication.targetOwnerId,
    targetOwnerLeaseGeneration: publication.targetOwnerLeaseGeneration
  };
}

function journalInventoryDigest(applicationPayload: Uint8Array): string {
  return createHash('sha256')
    .update(JOURNAL_INVENTORY_DOMAIN)
    .update(applicationPayload)
    .digest('hex');
}

function operationAggregateId(operationId: ZLinkActorJoinOperationId): string {
  const encoded = [operationId.high, operationId.low]
    .map(value => BigInt.asUintN(64, value).toString(16).padStart(16, '0'))
    .join('');
  return `${encoded.slice(0, 8)}-${encoded.slice(8, 12)}-${encoded.slice(12, 16)}`
    + `-${encoded.slice(16, 20)}-${encoded.slice(20)}`;
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
