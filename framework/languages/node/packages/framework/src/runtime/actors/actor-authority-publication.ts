import { createHash, randomBytes } from 'node:crypto';
import type {
  ActorRef
} from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations';
import type { ZLinkAuthoritySnapshot } from '../locations/internal-location-contracts';
import type { ZLinkObjectCreationStore } from '../locations/internal-store-contracts';
import {
  actorRelocationAuthorityApplicationPayload,
  decodeActorAuthorityPayload,
  encodeActorAuthorityPayload,
  relocatingActorAuthorityApplicationPayload,
  replaceActorRelocationAuthorityApplicationPayload,
  type ZLinkActorAuthorityState
} from './actor-authority-payload-codec';
import {
  replaceServiceRelocationAuthorityApplicationPayload,
  serviceRelocationAuthorityApplicationPayload
} from '../foundation/service-relocation-runtime';

const CREATION_OPERATION_TIMEOUT_MS = 30_000;

export interface ZLinkActorAuthorityIdentity {
  readonly actorType: string;
  readonly actor: ActorRef;
  readonly meshName: string;
  readonly ownerNodeGeneration: bigint;
  readonly owner: ZLinkLocationOwnerToken;
  readonly state?: ZLinkActorAuthorityState;
  readonly spotId: string;
  readonly spotGeneration: bigint;
  readonly spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User;
}

/**
 * Publishes the authority record that owns durable Actor lifecycle state.
 * The legacy location row remains the public lookup projection.
 */
export async function publishInitialActorAuthority(
  store: ZLinkObjectCreationStore,
  identity: ZLinkActorAuthorityIdentity,
  signal?: AbortSignal
): Promise<ZLinkAuthoritySnapshot> {
  const creatingPayload = encodeActorAuthorityIdentity({ ...identity, state: 'creating' });
  const readyPayload = encodeActorAuthorityIdentity({ ...identity, state: 'ready' });
  const digest = createHash('sha256').update(readyPayload).digest();
  const target = {
    meshName: identity.meshName,
    nodeRid: identity.actor.nodeRid,
    nodeLifecycleGeneration: identity.ownerNodeGeneration,
    owner: identity.owner
  };
  const reserved = await store.reserve({
    key: { kind: 'actor', globalId: identity.actor.actorId },
    intent: {
      stableType: identity.actorType,
      requestContentReference: `sha256:${digest.toString('hex')}`,
      requestSha256: digest,
      requestEncodedSize: BigInt(readyPayload.byteLength)
    },
    target,
    creatingPayload,
    capacity: { actors: 1, spots: 0 }
  }, signal);
  if (reserved.kind === 'alreadyExists') {
    requireActorAuthority(reserved.current, identity);
    return reserved.current;
  }
  if (reserved.kind !== 'reserved') {
    throw new Error(
      `Actor '${identity.actor.actorId}' authority reservation failed with '${reserved.kind}'.`
    );
  }

  try {
    const terminalEnvelope = Buffer.from(JSON.stringify({
      status: 'created',
      actorId: identity.actor.actorId,
      actorGeneration: identity.actor.objectGeneration.toString()
    }), 'utf8');
    const operationBytes = randomBytes(16);
    const completed = await store.completeCreation({
      key: { kind: 'actor', globalId: identity.actor.actorId },
      reservationId: reserved.reservationId,
      expectedStoreVersion: reserved.creating.storeVersion.value,
      target,
      completion: {
        kind: 'created',
        readyPayload,
        terminal: {
          operation: {
            sourceNodeRid: identity.actor.nodeRid,
            sourceNodeGeneration: identity.ownerNodeGeneration,
            operationId: {
              high: operationBytes.readBigUInt64BE(0),
              low: operationBytes.readBigUInt64BE(8)
            }
          },
          terminalEnvelope,
          terminalEnvelopeSha256: createHash('sha256').update(terminalEnvelope).digest(),
          operationDeadline: new Date(Date.now() + CREATION_OPERATION_TIMEOUT_MS)
        }
      }
    }, signal);
    if (completed.kind !== 'created') {
      throw new Error(
        `Actor '${identity.actor.actorId}' authority completion failed with '${completed.kind}'.`
      );
    }
    requireActorAuthority(completed.ready, identity);
    return completed.ready;
  } catch (error) {
    try {
      await store.abort({
        key: { kind: 'actor', globalId: identity.actor.actorId },
        reservationId: reserved.reservationId,
        expectedStoreVersion: reserved.creating.storeVersion.value,
        target
      }, signal);
    } catch {
      // A completed creation cannot be aborted; the validated authority remains.
    }
    throw error;
  }
}

export function encodeActorAuthorityIdentity(
  identity: ZLinkActorAuthorityIdentity
): Buffer {
  return encodeActorAuthorityPayload({
    state: identity.state ?? 'ready',
    stableType: identity.actorType,
    actorId: identity.actor.actorId,
    currentSpotId: identity.spotId,
    currentSpotGeneration: identity.spotGeneration,
    currentSpotKind: identity.spotKind,
    ownerId: identity.owner.ownerId,
    ownerLeaseGeneration: identity.owner.leaseGeneration,
    meshName: identity.meshName,
    nodeRid: identity.actor.nodeRid,
    nodeGeneration: identity.ownerNodeGeneration
  });
}

export function decodeActorAuthorityIdentity(
  payload: Uint8Array,
  objectGeneration: bigint
): ZLinkActorAuthorityIdentity | undefined {
  return decodeActorAuthorityIdentityCore(payload, objectGeneration, false);
}

export function decodeRelocatingActorAuthorityIdentity(
  payload: Uint8Array,
  objectGeneration: bigint
): ZLinkActorAuthorityIdentity | undefined {
  return decodeActorAuthorityIdentityCore(payload, objectGeneration, true);
}

function decodeActorAuthorityIdentityCore(
  payload: Uint8Array,
  objectGeneration: bigint,
  relocating: boolean
): ZLinkActorAuthorityIdentity | undefined {
  if (objectGeneration <= 0n) return undefined;
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  const actorApplication = relocating
    ? relocatingActorAuthorityApplicationPayload(outerApplication)
    : actorRelocationAuthorityApplicationPayload(outerApplication);
  if (actorApplication === undefined) return undefined;
  const value = decodeActorAuthorityPayload(actorApplication);
  if (value === undefined) return undefined;
  return {
    actorType: value.stableType,
    actor: {
      actorId: value.actorId,
      objectGeneration,
      meshName: value.meshName,
      nodeRid: value.nodeRid
    },
    meshName: value.meshName,
    ownerNodeGeneration: value.nodeGeneration,
    owner: {
      ownerId: value.ownerId,
      leaseGeneration: value.ownerLeaseGeneration
    },
    state: value.state,
    spotId: value.currentSpotId,
    spotGeneration: value.currentSpotGeneration,
    spotKind: value.currentSpotKind
  };
}

export function rewriteActorAuthorityRoute(
  payload: Uint8Array,
  actor: ActorRef,
  spotId: string,
  spotGeneration: bigint,
  spotKind: ZLinkSpotKind.Entry | ZLinkSpotKind.User,
  ownerNodeGeneration?: bigint,
  owner?: ZLinkLocationOwnerToken
): Buffer {
  return rewriteActorAuthorityPayload(payload, value => {
    if (value.actorId !== actor.actorId) {
      throw new Error(`Actor '${actor.actorId}' authority identity is invalid.`);
    }
    return encodeActorAuthorityPayload({
      ...value,
      currentSpotId: spotId,
      currentSpotGeneration: spotGeneration,
      currentSpotKind: spotKind,
      nodeRid: actor.nodeRid,
      nodeGeneration: ownerNodeGeneration ?? value.nodeGeneration,
      ownerId: owner?.ownerId ?? value.ownerId,
      ownerLeaseGeneration: owner?.leaseGeneration ?? value.ownerLeaseGeneration,
      meshName: actor.meshName ?? value.meshName
    });
  });
}

export function rewriteActorAuthorityOwner(
  payload: Uint8Array,
  owner: ZLinkLocationOwnerToken
): Buffer {
  return rewriteActorAuthorityPayload(payload, value => encodeActorAuthorityPayload({
    ...value,
    ownerId: owner.ownerId,
    ownerLeaseGeneration: owner.leaseGeneration
  }));
}

export function isActorAuthorityPayload(payload: Uint8Array): boolean {
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  return decodeActorAuthorityPayload(
    relocatingActorAuthorityApplicationPayload(outerApplication)
  ) !== undefined;
}

function rewriteActorAuthorityPayload(
  payload: Uint8Array,
  rewrite: (value: NonNullable<ReturnType<typeof decodeActorAuthorityPayload>>) => Buffer
): Buffer {
  const outerApplication = serviceRelocationAuthorityApplicationPayload(payload);
  const actorApplication = relocatingActorAuthorityApplicationPayload(outerApplication);
  const value = decodeActorAuthorityPayload(actorApplication);
  if (value === undefined) throw new Error('Actor authority payload identity is invalid.');
  const rewrittenActor = rewrite(value);
  const rewrittenPhase = replaceActorRelocationAuthorityApplicationPayload(
    outerApplication,
    rewrittenActor
  );
  return replaceServiceRelocationAuthorityApplicationPayload(payload, rewrittenPhase);
}

function requireActorAuthority(
  snapshot: ZLinkAuthoritySnapshot,
  expected: ZLinkActorAuthorityIdentity
): void {
  const actual = decodeActorAuthorityIdentity(snapshot.payload, snapshot.objectGeneration);
  if (
    snapshot.allocation.state !== 'active'
    || snapshot.allocation.objectKind !== 'actor'
    || snapshot.allocation.stableType !== expected.actorType
    || snapshot.allocation.descriptor.meshName !== expected.meshName
    || String(snapshot.allocation.descriptor.rid) !== String(expected.actor.nodeRid)
    || snapshot.allocation.descriptorLifecycleGeneration !== expected.ownerNodeGeneration
    || snapshot.ownerId !== expected.owner.ownerId
    || snapshot.ownerLeaseGeneration !== expected.owner.leaseGeneration
    || actual === undefined
    || actual.actorType !== expected.actorType
    || actual.actor.actorId !== expected.actor.actorId
    || actual.actor.objectGeneration !== expected.actor.objectGeneration
    || String(actual.actor.nodeRid) !== String(expected.actor.nodeRid)
  ) {
    throw new Error(`Actor '${expected.actor.actorId}' authority fence does not match its ActorRef.`);
  }
}
