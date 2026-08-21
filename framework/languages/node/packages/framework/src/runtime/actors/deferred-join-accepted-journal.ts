import { createHash } from 'node:crypto';
import type {
  ActorRef,
  ZLinkActor,
  ZLinkActorJoinCompletion,
  ZLinkActorJoinOperationId,
  ZLinkBlobReference,
  ZLinkMessageSerializer,
  ZLinkRelocationStore
} from '../../contracts';
import type { ZLinkAuthoritySnapshot } from '../locations/internal-location-contracts';
import type { ZLinkAuthorityStore } from '../locations/internal-store-contracts';
import {
  ZLinkEncodedPayload,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkMessage
} from '../../contracts';
import { encodeAuthorityKey } from '../locations/authority-key-codec';
import {
  crc32c,
  ServiceRelocationAuthorityPayloadCodec,
  replaceServiceRelocationAuthorityApplicationPayload,
  serviceRelocationAuthorityApplicationPayload,
  serviceRelocationAuthoritySlotIdentity,
  type ServiceRelocationPublication
} from '../foundation/service-relocation-runtime';
import { putNewRelocationBlob } from '../locations/relocation-blob';
import {
  decodeRoutingId,
  encodeRoutingIdStorageHex,
  routingIdWireHex
} from '../routing-id';
import {
  decodeRelocatingActorAuthorityIdentity,
  encodeActorAuthorityIdentity
} from './actor-authority-publication';
import {
  replaceActorRelocationAuthorityApplicationPayload
} from './actor-authority-payload-codec';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { wrapFrameworkPayloadMessage } from '../messaging/payload-codec';

const RETENTION_MS = 24 * 60 * 60 * 1_000;
const ROOT_VERSION = 2;
const ROOT_MAGIC = Buffer.from('ZLJR');
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
  readonly replyContentType?: string;
  readonly cursor: ZLinkDeferredJoinDeliveryCursor;
}

export interface ZLinkDeferredJoinRootIdentity {
  readonly authorityKey: string;
  readonly objectKind: 'actor' | 'user_spot' | 'instance_spot';
  readonly objectGeneration: bigint;
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
}

/** Identifies whether an authority slot points at a deferred Join journal root. */
export async function isDeferredJoinAcceptedRootPublication(
  relocation: ZLinkRelocationStore,
  reference: string,
  checksumCrc32c: number,
  expected: ZLinkDeferredJoinRootIdentity,
  signal?: AbortSignal
): Promise<boolean> {
  if (expected.objectKind !== 'actor' || expected.aggregateGeneration === 0n) return false;
  const read = await relocation.read({ value: reference } as ZLinkBlobReference, signal);
  if (read.kind !== 'found' || crc32c(read.bytes) !== checksumCrc32c) return false;
  if (hasJournalRootMagic(read.bytes)) {
    return false;
  }
  const root = await readCanonicalDeferredJoinRoot(relocation, read.bytes, signal);
  return root !== undefined
    && root.identity.aggregateId === expected.aggregateId
    && root.identity.aggregateGeneration === expected.aggregateGeneration
    && root.identity.authorityKey === expected.authorityKey
    && root.identity.objectKind === expected.objectKind
    && root.identity.objectGeneration === expected.objectGeneration;
}

interface DeferredJoinAuthorityPublication {
  readonly applicationPayload: Buffer;
  readonly reference: ZLinkBlobReference;
  readonly checksumCrc32c: number;
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly targetOwnerId: string;
  readonly targetOwnerLeaseGeneration: bigint;
  readonly canonical?: boolean;
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
    private readonly relocation: ZLinkRelocationStore,
    private readonly messageSerializers?: ReadonlyMap<string, ZLinkMessageSerializer>
  ) {}

  async prepare(
    actorId: string,
    operationId: ZLinkActorJoinOperationId,
    actor: ActorRef,
    rawReply: Uint8Array,
    replyContentType?: string,
    signal?: AbortSignal,
    canonicalInventoryDigest?: string
  ): Promise<ZLinkDeferredJoinAcceptedRoot> {
    validateIdentity(actorId, operationId, actor);
    if (replyContentType !== undefined && replyContentType.length === 0) {
      throw new TypeError('Deferred Join reply content type must be non-empty.');
    }
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

    const rootValue = {
      operationId,
      actor,
      rawReply: Buffer.from(rawReply),
      ...(replyContentType === undefined ? {} : { replyContentType }),
      cursor: 'prepared'
    } as const;
    const initialPublication = decodeAuthorityPublication(read.payload);
    const initialCanonicalIdentity = serviceRelocationAuthoritySlotIdentity(read.payload);
    const canonicalPublication: DeferredJoinAuthorityPublication | undefined =
      initialPublication?.canonical === true
        ? initialPublication
        : initialCanonicalIdentity === undefined
          ? undefined
          : {
              applicationPayload: serviceRelocationAuthorityApplicationPayload(read.payload),
              reference: { value: 'zlink-direct:unpublished' } as ZLinkBlobReference,
              checksumCrc32c: 0,
              aggregateId: initialCanonicalIdentity.aggregateId,
              aggregateGeneration: requireCanonicalAggregateGeneration(
                initialCanonicalIdentity.aggregateGeneration
              ),
              targetOwnerId: read.ownerId,
              targetOwnerLeaseGeneration: read.ownerLeaseGeneration,
              canonical: true
            };
    const root = canonicalPublication !== undefined
      && (canonicalInventoryDigest !== undefined
        || !canonicalPublication.reference.value.startsWith('zlink-direct:'))
      ? await this.storeCanonicalRoot(
          rootValue,
          read,
          canonicalPublication,
          signal,
          canonicalInventoryDigest
        )
      : await this.storeRoot(rootValue, signal);
    if (initialCanonicalIdentity !== undefined && canonicalInventoryDigest !== undefined) {
      return {
        ...root,
        authority: read
      };
    }
    let expected: ZLinkAuthoritySnapshot = read;
    for (let attempt = 0; attempt < 3; attempt++) {
      const currentPublication = decodeAuthorityPublication(expected.payload);
      const canonicalIdentity = serviceRelocationAuthoritySlotIdentity(expected.payload);
      const publication: DeferredJoinAuthorityPublication = {
        applicationPayload: Buffer.from(
          currentPublication?.applicationPayload
            ?? serviceRelocationAuthorityApplicationPayload(expected.payload)
        ),
        reference: root.reference,
        checksumCrc32c: root.checksumCrc32c,
        aggregateId: canonicalIdentity !== undefined
          ? canonicalIdentity.aggregateId
          : operationAggregateId(operationId),
        aggregateGeneration: canonicalIdentity === undefined
          ? actor.objectGeneration
          : requireCanonicalAggregateGeneration(canonicalIdentity.aggregateGeneration),
        targetOwnerId: expected.ownerId,
        targetOwnerLeaseGeneration: expected.ownerLeaseGeneration
      };
      const result = await this.authority.compareExchangeAuthority(
        key,
        expected.storeVersion,
        {
          kind: 'put',
          generationTransition: 'preserve',
          payload: currentPublication === undefined && canonicalIdentity !== undefined
            ? new ServiceRelocationAuthorityPayloadCodec().publish(
                expected.payload,
                toServiceRelocationPublication(publication)
              )
            : replaceDeferredJoinAuthorityPublication(
                expected.payload,
                encodeAuthorityPublication(publication)
              )
        },
        signal
      );
      if (result.kind === 'stored') {
        if (existing !== undefined) {
          await this.deleteBestEffort(existing.reference);
        }
        return {
          ...root,
          authority: storedSnapshot(result)
        };
      }
      if (result.kind !== 'conflict' || result.current.kind !== 'snapshot') break;
      const current = await this.readPublished(result.current, signal);
      if (current !== undefined) {
        await this.deleteBestEffort(root.reference);
        requireSameOperation(current, operationId, actor);
        return current;
      }
      requireAuthorityActor(result.current.payload, actor);
      expected = result.current;
    }
    await this.deleteBestEffort(root.reference);
    throw new Error(`Actor '${actorId}' authority rejected Join completion publication.`);
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
          : current.replyContentType === undefined
            ? ZLinkMessage.fromEncoded(ZLinkEncodedPayload.from(current.rawReply))
            : wrapFrameworkPayloadMessage(
                RuntimeMessage.from(current.rawReply),
                this.messageSerializers,
                current.replyContentType,
                undefined,
                'reply'
              )
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
        ...(current.replyContentType === undefined
          ? {}
          : { replyContentType: current.replyContentType }),
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
    if (read.kind !== 'found' && publication.canonical === true
      && publication.reference.value.startsWith('zlink-direct:')) {
      return undefined;
    }
    if (
      read.kind !== 'found'
      || crc32c(read.bytes) !== publication.checksumCrc32c
    ) {
      throw new Error('Published deferred Join completion root is missing or corrupt.');
    }
    if (publication.canonical === true && !hasJournalRootMagic(read.bytes)) {
      const root = await readCanonicalDeferredJoinRoot(
        this.relocation,
        read.bytes,
        signal
      );
      if (root === undefined) {
        // A canonical relocation root and the post-commit Join journal share
        // the embedded slot. A root without a completion remains Host-owned.
        return undefined;
      }
      if (root.identity.aggregateId !== publication.aggregateId
        || root.identity.aggregateGeneration !== publication.aggregateGeneration
        || root.identity.objectGeneration !== authority.objectGeneration) {
        throw new Error('Published deferred Join completion root identity does not match its authority slot.');
      }
      return {
        authority,
        reference: publication.reference,
        checksumCrc32c: publication.checksumCrc32c,
        ...root.completion
      };
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

  private async storeCanonicalRoot(
    value: Omit<
      ZLinkDeferredJoinAcceptedRoot,
      'authority' | 'reference' | 'checksumCrc32c'
    >,
    authority: ZLinkAuthoritySnapshot,
    publication: DeferredJoinAuthorityPublication,
    signal?: AbortSignal,
    canonicalInventoryDigest?: string
  ): Promise<Omit<ZLinkDeferredJoinAcceptedRoot, 'authority'>> {
    let inventory: Buffer;
    if (canonicalInventoryDigest !== undefined) {
      if (!/^[0-9a-f]{64}$/u.test(canonicalInventoryDigest)) {
        throw new Error('Canonical relocation inventory digest is invalid.');
      }
      inventory = Buffer.from(canonicalInventoryDigest, 'hex');
    } else {
      const current = await this.relocation.read(publication.reference, signal);
      if (current.kind !== 'found'
        || crc32c(current.bytes) !== publication.checksumCrc32c) {
        throw new Error('Canonical relocation manifest is missing or corrupt.');
      }
      inventory = decodeCanonicalTreeManifest(current.bytes).inventoryDigest;
    }
    const logicalRoot = encodeCanonicalDeferredJoinRoot(
      value,
      authority,
      publication,
      inventory
    );
    const stored = await storeCanonicalTree(
      this.relocation,
      logicalRoot,
      inventory,
      signal
    );
    return {
      ...value,
      rawReply: Buffer.from(value.rawReply),
      reference: stored.reference,
      checksumCrc32c: stored.checksumCrc32c
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
  const encoded = Buffer.concat([ROOT_MAGIC, Buffer.from(JSON.stringify({
    version: ROOT_VERSION,
    operationHigh: value.operationId.high.toString(),
    operationLow: value.operationId.low.toString(),
    actorId: value.actor.actorId,
    actorGeneration: value.actor.objectGeneration.toString(),
    actorMeshName: value.actor.meshName,
    actorNodeRid: String(value.actor.nodeRid),
    actorNodeRidHex: nodeRidHex,
    rawReply: Buffer.from(value.rawReply).toString('base64'),
    ...(value.replyContentType === undefined
      ? {}
      : { replyContentType: value.replyContentType }),
    cursor: value.cursor
  }), 'utf8')]);
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
  const bytes = Buffer.from(payload);
  const encoded = hasJournalRootMagic(bytes)
    ? bytes.subarray(ROOT_MAGIC.byteLength)
    : bytes;
  const value = JSON.parse(encoded.toString('utf8')) as Record<string, unknown>;
  if (
    value.version !== ROOT_VERSION
    || typeof value.operationHigh !== 'string'
    || typeof value.operationLow !== 'string'
    || typeof value.actorId !== 'string'
    || typeof value.actorGeneration !== 'string'
    || typeof value.actorMeshName !== 'string'
    || typeof value.actorNodeRid !== 'string'
    || typeof value.rawReply !== 'string'
    || (value.replyContentType !== undefined
      && (typeof value.replyContentType !== 'string' || value.replyContentType.length === 0))
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
    ...(typeof value.replyContentType === 'string'
      ? { replyContentType: value.replyContentType }
      : {}),
    cursor: value.cursor
  };
}

function encodeAuthorityPublication(value: DeferredJoinAuthorityPublication): Buffer {
  return Buffer.from(new ServiceRelocationAuthorityPayloadCodec().publish(
    value.applicationPayload,
    toServiceRelocationPublication(value)
  ));
}

function toServiceRelocationPublication(
  value: DeferredJoinAuthorityPublication
): ServiceRelocationPublication {
  return {
    reference: value.reference.value,
    checksumCrc32c: value.checksumCrc32c,
    aggregateId: value.aggregateId,
    aggregateGeneration: value.aggregateGeneration,
    inventoryDigest: journalInventoryDigest(value.applicationPayload),
    targetOwnerId: value.targetOwnerId,
    targetOwnerLeaseGeneration: value.targetOwnerLeaseGeneration
  };
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
  const codec = new ServiceRelocationAuthorityPayloadCodec();
  const currentPublication = codec.read(payload);
  const replacementPublication = codec.read(replacement);
  if (
    currentPublication?.canonical === true
    || replacementPublication?.canonical === true
  ) {
    // Canonical authority has exactly one embedded relocation slot. A journal
    // cursor replaces that slot atomically; it must never be wrapped or nested
    // under an older ZLAR/application layer.
    return Buffer.from(replacement);
  }
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
  return publication.canonical === true
    || publication.inventoryDigest === journalInventoryDigest(applicationPayload)
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
    targetOwnerLeaseGeneration: publication.targetOwnerLeaseGeneration,
    canonical: publication.canonical
  };
}

function hasJournalRootMagic(payload: Uint8Array): boolean {
  const bytes = Buffer.from(payload);
  return bytes.byteLength >= ROOT_MAGIC.byteLength
    && bytes.subarray(0, ROOT_MAGIC.byteLength).equals(ROOT_MAGIC);
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

function requireCanonicalAggregateGeneration(value: bigint): bigint {
  if (value === 0n) {
    throw new ZLinkFrameworkException(
      ZLinkFrameworkErrorKind.DataLost,
      'Canonical relocation slot has no aggregate generation.'
    );
  }
  return value;
}

interface CanonicalTreeManifest {
  readonly inventoryDigest: Buffer;
  readonly logicalLength: bigint;
  readonly logicalChecksumCrc32c: number;
  readonly chunks: readonly {
    readonly order: number;
    readonly reference: ZLinkBlobReference;
    readonly length: bigint;
    readonly checksumCrc32c: number;
  }[];
}

function decodeCanonicalTreeManifest(payload: Uint8Array): CanonicalTreeManifest {
  const body = decodeCanonicalTreeFrame(payload, 'ZLTM');
  let offset = 0;
  const take = (length: number): Buffer => {
    if (length < 0 || offset + length > body.byteLength) {
      throw new Error('Canonical relocation manifest is truncated.');
    }
    const result = body.subarray(offset, offset + length);
    offset += length;
    return result;
  };
  if (take(1)[0] !== 1) throw new Error('Canonical relocation manifest version is invalid.');
  const logicalLength = take(8).readBigUInt64BE();
  const logicalChecksumCrc32c = take(4).readUInt32BE();
  if (take(1)[0] !== 32) throw new Error('Canonical relocation inventory digest is invalid.');
  const inventoryDigest = Buffer.from(take(32));
  const count = take(4).readUInt32BE();
  if (count === 0 || count > 4096) throw new Error('Canonical relocation chunk count is invalid.');
  const chunks = Array.from({ length: count }, (_, index) => {
    const order = take(4).readUInt32BE();
    const referenceLength = take(2).readUInt16BE();
    if (order !== index || referenceLength === 0 || referenceLength > 4096) {
      throw new Error('Canonical relocation chunk identity is invalid.');
    }
    const reference = {
      value: new TextDecoder('utf-8', { fatal: true }).decode(take(referenceLength))
    } as ZLinkBlobReference;
    const length = take(8).readBigUInt64BE();
    const checksumCrc32c = take(4).readUInt32BE();
    return { order, reference, length, checksumCrc32c };
  });
  if (offset !== body.byteLength
    || chunks.reduce((total, value) => total + value.length, 0n) !== logicalLength) {
    throw new Error('Canonical relocation manifest length is invalid.');
  }
  return { inventoryDigest, logicalLength, logicalChecksumCrc32c, chunks };
}

function decodeCanonicalTreeFrame(payload: Uint8Array, magic: string): Buffer {
  const bytes = Buffer.from(payload);
  if (bytes.byteLength < 15
    || !bytes.subarray(0, 4).equals(Buffer.from(magic))
    || bytes[4] !== 1
    || bytes.readUInt16BE(5) !== 0) {
    throw new Error(`Canonical relocation ${magic} frame header is invalid.`);
  }
  const bodyLength = bytes.readUInt32BE(7);
  if (bytes.byteLength !== 11 + bodyLength + 4
    || bytes.readUInt32BE(bytes.byteLength - 4)
      !== crc32c(bytes.subarray(0, bytes.byteLength - 4))) {
    throw new Error(`Canonical relocation ${magic} frame checksum is invalid.`);
  }
  return bytes.subarray(11, 11 + bodyLength);
}

function encodeCanonicalTreeFrame(magic: string, body: Uint8Array): Buffer {
  const header = Buffer.alloc(11);
  header.write(magic, 0, 'ascii');
  header[4] = 1;
  header.writeUInt16BE(0, 5);
  header.writeUInt32BE(body.byteLength, 7);
  const frame = Buffer.concat([header, Buffer.from(body)]);
  return Buffer.concat([frame, u32be(crc32c(frame))]);
}

async function storeCanonicalTree(
  store: ZLinkRelocationStore,
  logicalRoot: Uint8Array,
  inventoryDigest: Uint8Array,
  signal?: AbortSignal
): Promise<{ readonly reference: ZLinkBlobReference; readonly checksumCrc32c: number }> {
  const logical = Buffer.from(logicalRoot);
  const chunkBody = Buffer.concat([u32be(0), u32be(logical.byteLength), logical]);
  const chunk = encodeCanonicalTreeFrame('ZLTC', chunkBody);
  const storedChunk = await putAndVerifyCanonicalBlob(store, chunk, signal);
  const referenceBytes = Buffer.from(storedChunk.reference.value, 'utf8');
  const manifestBody = Buffer.concat([
    Buffer.of(1),
    u64be(BigInt(logical.byteLength)),
    u32be(crc32c(logical)),
    Buffer.of(32), Buffer.from(inventoryDigest),
    u32be(1), u32be(0), u16be(referenceBytes.byteLength), referenceBytes,
    u64be(BigInt(logical.byteLength)), u32be(crc32c(logical))
  ]);
  const manifest = encodeCanonicalTreeFrame('ZLTM', manifestBody);
  const storedManifest = await putAndVerifyCanonicalBlob(store, manifest, signal);
  return {
    reference: storedManifest.reference,
    checksumCrc32c: crc32c(manifest)
  };
}

async function putAndVerifyCanonicalBlob(
  store: ZLinkRelocationStore,
  payload: Uint8Array,
  signal?: AbortSignal
): Promise<{ readonly reference: ZLinkBlobReference }> {
  const stored = await putNewRelocationBlob(store, payload, RETENTION_MS, signal);
  const read = await store.read(stored.reference, signal);
  if (read.kind !== 'found' || !Buffer.from(read.bytes).equals(Buffer.from(payload))) {
    await store.delete(stored.reference).catch(() => undefined);
    throw new Error('Relocation Store failed canonical tree verification.');
  }
  return { reference: stored.reference };
}

async function readCanonicalDeferredJoinRoot(
  store: ZLinkRelocationStore,
  manifestPayload: Uint8Array,
  signal?: AbortSignal
): Promise<{
  readonly identity: ZLinkDeferredJoinRootIdentity;
  readonly completion: Omit<
    ZLinkDeferredJoinAcceptedRoot,
    'authority' | 'reference' | 'checksumCrc32c'
  >;
} | undefined> {
  let manifest: CanonicalTreeManifest;
  try {
    manifest = decodeCanonicalTreeManifest(manifestPayload);
  } catch {
    return undefined;
  }
  const parts: Buffer[] = [];
  for (const expected of manifest.chunks) {
    const read = await store.read(expected.reference, signal);
    if (read.kind !== 'found') throw new Error('Canonical relocation chunk is missing.');
    const body = decodeCanonicalTreeFrame(read.bytes, 'ZLTC');
    if (body.byteLength < 8
      || body.readUInt32BE(0) !== expected.order
      || BigInt(body.readUInt32BE(4)) !== expected.length) {
      throw new Error('Canonical relocation chunk identity is invalid.');
    }
    const data = body.subarray(8);
    if (crc32c(data) !== expected.checksumCrc32c) {
      throw new Error('Canonical relocation chunk data checksum is invalid.');
    }
    parts.push(data);
  }
  const logical = Buffer.concat(parts);
  if (BigInt(logical.byteLength) !== manifest.logicalLength
    || crc32c(logical) !== manifest.logicalChecksumCrc32c) {
    throw new Error('Canonical relocation logical root checksum is invalid.');
  }
  return decodeCanonicalDeferredJoinRoot(logical);
}

function encodeCanonicalDeferredJoinRoot(
  value: Omit<ZLinkDeferredJoinAcceptedRoot, 'authority' | 'reference' | 'checksumCrc32c'>,
  authority: ZLinkAuthoritySnapshot,
  publication: DeferredJoinAuthorityPublication,
  inventoryDigest: Uint8Array
): Buffer {
  const key = encodeAuthorityKey('actor', value.actor.actorId).value;
  const completion = encodeDotNetDeferredJoinCompletion(value);
  return Buffer.concat([
    u32le(0x5a4c5231), u16le(2), dotNetGuidBytes(publication.aggregateId),
    u64le(publication.aggregateGeneration), bytes32le(inventoryDigest), u32le(1),
    text16le(key), Buffer.of(1), u64le(authority.objectGeneration),
    u64le(authority.authorityOwnerGeneration), bytes32le(Buffer.alloc(0)),
    u32le(0), u32le(0), bytes32le(Buffer.alloc(0)), bytes32le(completion)
  ]);
}

function decodeCanonicalDeferredJoinRoot(
  payload: Uint8Array
): {
  readonly identity: ZLinkDeferredJoinRootIdentity;
  readonly completion: Omit<
    ZLinkDeferredJoinAcceptedRoot,
    'authority' | 'reference' | 'checksumCrc32c'
  >;
} | undefined {
  const reader = new LittleEndianReader(payload);
  if (reader.u32() !== 0x5a4c5231 || reader.u16() !== 2) return undefined;
  const aggregateId = canonicalUuidFromDotNetBytes(reader.take(16));
  const aggregateGeneration = reader.u64();
  reader.bytes32();
  const count = reader.u32();
  let completion: ReturnType<typeof decodeDotNetDeferredJoinCompletion> | undefined;
  let completionIdentity: Pick<
    ZLinkDeferredJoinRootIdentity,
    'authorityKey' | 'objectKind' | 'objectGeneration'
  > | undefined;
  for (let index = 0; index < count; index++) {
    const authorityKey = reader.text16();
    const objectKind = reader.u8();
    const objectGeneration = reader.u64();
    reader.u64(); reader.bytes32();
    const jobs = reader.u32();
    for (let job = 0; job < jobs; job++) { reader.u64(); reader.bytes32(); }
    const timers = reader.u32();
    for (let timer = 0; timer < timers; timer++) {
      reader.text16(); reader.i64(); reader.i64(); reader.bytes32();
    }
    reader.bytes32();
    const encodedCompletion = reader.bytes32();
    if (encodedCompletion.byteLength !== 0) {
      if (completion !== undefined) throw new Error('Canonical relocation root has duplicate completions.');
      completion = decodeDotNetDeferredJoinCompletion(encodedCompletion);
      if (objectKind !== 1) return undefined;
      completionIdentity = {
        authorityKey,
        objectKind: 'actor',
        objectGeneration
      };
    }
  }
  if (!reader.done) throw new Error('Canonical relocation root has trailing bytes.');
  if (completion === undefined || completionIdentity === undefined) return undefined;
  if (completion.actor.objectGeneration !== completionIdentity.objectGeneration
    || encodeAuthorityKey('actor', completion.actor.actorId).value
      !== completionIdentity.authorityKey) {
    return undefined;
  }
  return {
    identity: {
      ...completionIdentity,
      aggregateId,
      aggregateGeneration
    },
    completion
  };
}

function encodeDotNetDeferredJoinCompletion(
  value: Omit<ZLinkDeferredJoinAcceptedRoot, 'authority' | 'reference' | 'checksumCrc32c'>
): Buffer {
  const routingId = Buffer.from(encodeRoutingIdStorageHex(value.actor.nodeRid), 'hex');
  return Buffer.concat([
    u32le(0x5a4c4a43), Buffer.of(2), text16le(value.actor.actorId),
    u64le(value.actor.objectGeneration), u64le(value.operationId.high), u64le(value.operationId.low),
    text16le(value.actor.meshName), Buffer.of(routingId.byteLength), routingId,
    u64le(value.actor.objectGeneration),
    Buffer.of(value.replyContentType === undefined ? 0 : 1),
    ...(value.replyContentType === undefined ? [] : [text16le(value.replyContentType)]),
    bytes32le(value.rawReply),
    Buffer.of(cursorIndex(value.cursor) + 1)
  ]);
}

function decodeDotNetDeferredJoinCompletion(
  payload: Uint8Array
): Omit<ZLinkDeferredJoinAcceptedRoot, 'authority' | 'reference' | 'checksumCrc32c'> {
  const reader = new LittleEndianReader(payload);
  if (reader.u32() !== 0x5a4c4a43 || reader.u8() !== 2) {
    throw new Error('Canonical deferred Join completion header is invalid.');
  }
  const actorId = reader.text16();
  const objectGeneration = reader.u64();
  const operationId = { high: reader.u64(), low: reader.u64() };
  const meshName = reader.text16();
  const nodeBytes = reader.take(reader.u8());
  const actorGeneration = reader.u64();
  const contentTypePresence = reader.u8();
  if (contentTypePresence > 1) {
    throw new Error('Canonical deferred Join completion content type is invalid.');
  }
  const replyContentType = contentTypePresence === 1 ? reader.text16() : undefined;
  const rawReply = reader.bytes32();
  const cursorValue = reader.u8();
  if (!reader.done || actorGeneration !== objectGeneration || cursorValue < 1 || cursorValue > 3) {
    throw new Error('Canonical deferred Join completion is invalid.');
  }
  const actor: ActorRef = {
    actorId,
    objectGeneration,
    meshName,
    nodeRid: decodeRoutingId('', nodeBytes.toString('hex'))
  };
  validateIdentity(actorId, operationId, actor);
  return {
    operationId,
    actor,
    rawReply,
    ...(replyContentType === undefined ? {} : { replyContentType }),
    cursor: (['prepared', 'committed', 'delivered'] as const)[cursorValue - 1]!
  };
}

class LittleEndianReader {
  private offset = 0;
  private readonly bytes: Buffer;
  constructor(payload: Uint8Array) { this.bytes = Buffer.from(payload); }
  get done(): boolean { return this.offset === this.bytes.byteLength; }
  take(length: number): Buffer {
    if (length < 0 || this.offset + length > this.bytes.byteLength) {
      throw new Error('Canonical relocation root is truncated.');
    }
    const result = this.bytes.subarray(this.offset, this.offset + length);
    this.offset += length;
    return result;
  }
  u8(): number { return this.take(1)[0]!; }
  u16(): number { return this.take(2).readUInt16LE(); }
  u32(): number { return this.take(4).readUInt32LE(); }
  u64(): bigint { return this.take(8).readBigUInt64LE(); }
  i64(): bigint { return this.take(8).readBigInt64LE(); }
  text16(): string {
    const bytes = this.take(this.u16());
    return new TextDecoder('utf-8', { fatal: true }).decode(bytes);
  }
  bytes32(): Buffer { return this.take(this.u32()); }
}

function dotNetGuidBytes(value: string): Buffer {
  const bytes = Buffer.from(value.replaceAll('-', ''), 'hex');
  return Buffer.from([
    bytes[3]!, bytes[2]!, bytes[1]!, bytes[0]!,
    bytes[5]!, bytes[4]!, bytes[7]!, bytes[6]!,
    ...bytes.subarray(8)
  ]);
}

function canonicalUuidFromDotNetBytes(value: Uint8Array): string {
  const bytes = Buffer.from(value);
  if (bytes.byteLength !== 16) {
    throw new TypeError('Canonical relocation aggregate id is invalid.');
  }
  const canonical = Buffer.from([
    bytes[3]!, bytes[2]!, bytes[1]!, bytes[0]!,
    bytes[5]!, bytes[4]!,
    bytes[7]!, bytes[6]!,
    ...bytes.subarray(8)
  ]).toString('hex');
  return `${canonical.slice(0, 8)}-${canonical.slice(8, 12)}-${canonical.slice(12, 16)}`
    + `-${canonical.slice(16, 20)}-${canonical.slice(20)}`;
}

function text16le(value: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength === 0 || bytes.byteLength > 0xffff) throw new Error('Text16 is invalid.');
  return Buffer.concat([u16le(bytes.byteLength), bytes]);
}

function bytes32le(value: Uint8Array): Buffer {
  return Buffer.concat([u32le(value.byteLength), Buffer.from(value)]);
}

function u16le(value: number): Buffer {
  const result = Buffer.alloc(2); result.writeUInt16LE(value); return result;
}

function u32le(value: number): Buffer {
  const result = Buffer.alloc(4); result.writeUInt32LE(value); return result;
}

function u64le(value: bigint): Buffer {
  const result = Buffer.alloc(8); result.writeBigUInt64LE(value); return result;
}

function u16be(value: number): Buffer {
  const result = Buffer.alloc(2); result.writeUInt16BE(value); return result;
}

function u32be(value: number): Buffer {
  const result = Buffer.alloc(4); result.writeUInt32BE(value); return result;
}

function u64be(value: bigint): Buffer {
  const result = Buffer.alloc(8); result.writeBigUInt64BE(value); return result;
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
