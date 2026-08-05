import { createHash, randomBytes } from 'node:crypto';
import type {
  ActorRef
} from '../../contracts';
import type {
  ZLinkAuthoritySnapshot,
  ZLinkLocationOwnerToken
} from '../../contracts/Locations';
import type { ZLinkObjectCreationStore } from '../locations/internal-store-contracts';

const ACTOR_AUTHORITY_VERSION = 1;
const CREATION_OPERATION_TIMEOUT_MS = 30_000;

export interface ZLinkActorAuthorityIdentity {
  readonly actorType: string;
  readonly actor: ActorRef;
  readonly meshName: string;
  readonly ownerNodeGeneration: bigint;
  readonly owner: ZLinkLocationOwnerToken;
  readonly spotId?: string;
  readonly spotGeneration?: bigint;
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
  const readyPayload = encodeActorAuthorityIdentity(identity);
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
    creatingPayload: readyPayload,
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
  return Buffer.from(JSON.stringify({
    version: ACTOR_AUTHORITY_VERSION,
    actorType: identity.actorType,
    actorId: identity.actor.actorId,
    actorNodeRid: String(identity.actor.nodeRid),
    actorGeneration: identity.actor.objectGeneration.toString(),
    meshName: identity.meshName,
    ownerNodeGeneration: identity.ownerNodeGeneration.toString(),
    ownerId: identity.owner.ownerId,
    ownerLeaseGeneration: identity.owner.leaseGeneration.toString(),
    spotId: identity.spotId,
    spotGeneration: identity.spotGeneration?.toString()
  }), 'utf8');
}

export function decodeActorAuthorityIdentity(
  payload: Uint8Array
): ZLinkActorAuthorityIdentity | undefined {
  let value: Record<string, unknown>;
  try {
    value = JSON.parse(Buffer.from(payload).toString('utf8')) as Record<string, unknown>;
    for (let depth = 0; depth < 4; depth += 1) {
      const nested = (
        (value.magic === 'ZLAP' || value.magic === 'ZLAR')
        && value.version === 1
        && typeof value.base === 'string'
      )
        ? value.base
        : typeof value.applicationPayload === 'string'
          ? value.applicationPayload
          : undefined;
      if (nested === undefined) break;
      value = JSON.parse(Buffer.from(nested, 'base64').toString('utf8')) as Record<string, unknown>;
    }
  } catch {
    return undefined;
  }
  if (
    value.version !== ACTOR_AUTHORITY_VERSION
    || typeof value.actorType !== 'string'
    || typeof value.actorId !== 'string'
    || typeof value.actorNodeRid !== 'string'
    || typeof value.actorGeneration !== 'string'
    || typeof value.meshName !== 'string'
    || typeof value.ownerNodeGeneration !== 'string'
    || typeof value.ownerId !== 'string'
    || typeof value.ownerLeaseGeneration !== 'string'
  ) {
    return undefined;
  }
  const actorGeneration = BigInt(value.actorGeneration);
  const ownerNodeGeneration = BigInt(value.ownerNodeGeneration);
  const ownerLeaseGeneration = BigInt(value.ownerLeaseGeneration);
  if (
    value.actorId.length === 0
    || actorGeneration <= 0n
    || ownerNodeGeneration <= 0n
    || ownerLeaseGeneration <= 0n
  ) {
    return undefined;
  }
  return {
    actorType: value.actorType,
    actor: {
      actorId: value.actorId,
      objectGeneration: actorGeneration,
      meshName: value.meshName,
      nodeRid: value.actorNodeRid as ActorRef['nodeRid']
    },
    meshName: value.meshName,
    ownerNodeGeneration,
    owner: {
      ownerId: value.ownerId,
      leaseGeneration: ownerLeaseGeneration
    },
    spotId: typeof value.spotId === 'string' ? value.spotId : undefined,
    spotGeneration: typeof value.spotGeneration === 'string'
      ? BigInt(value.spotGeneration)
      : undefined
  };
}

export function rewriteActorAuthorityRoute(
  payload: Uint8Array,
  actor: ActorRef,
  spotId: string,
  spotGeneration: bigint | undefined,
  ownerNodeGeneration?: bigint,
  owner?: ZLinkLocationOwnerToken
): Buffer {
  const rewrite = (encoded: Buffer, depth: number): Buffer => {
    if (depth > 4) throw new Error('Actor authority payload nesting exceeds the supported bound.');
    const value = JSON.parse(encoded.toString('utf8')) as Record<string, unknown>;
    const nestedKey = (
      (value.magic === 'ZLAP' || value.magic === 'ZLAR')
      && value.version === 1
      && typeof value.base === 'string'
    )
      ? 'base'
      : typeof value.applicationPayload === 'string'
        ? 'applicationPayload'
        : undefined;
    if (nestedKey !== undefined) {
      value[nestedKey] = rewrite(
        Buffer.from(value[nestedKey] as string, 'base64'),
        depth + 1
      ).toString('base64');
      return Buffer.from(JSON.stringify(value));
    }
    if (value.version !== ACTOR_AUTHORITY_VERSION || value.actorId !== actor.actorId) {
      throw new Error(`Actor '${actor.actorId}' authority identity is invalid.`);
    }
    value.actorNodeRid = String(actor.nodeRid);
    value.actorGeneration = actor.objectGeneration.toString();
    value.spotId = spotId;
    value.spotGeneration = spotGeneration?.toString();
    if (ownerNodeGeneration !== undefined) {
      value.ownerNodeGeneration = ownerNodeGeneration.toString();
    }
    if (owner !== undefined) {
      value.ownerId = owner.ownerId;
      value.ownerLeaseGeneration = owner.leaseGeneration.toString();
    }
    return Buffer.from(JSON.stringify(value));
  };
  return rewrite(Buffer.from(payload), 0);
}

export function rewriteActorAuthorityOwner(
  payload: Uint8Array,
  owner: ZLinkLocationOwnerToken
): Buffer {
  const rewrite = (encoded: Buffer, depth: number): Buffer => {
    if (depth > 4) throw new Error('Actor authority payload nesting exceeds the supported bound.');
    const value = JSON.parse(encoded.toString('utf8')) as Record<string, unknown>;
    const nestedKey = (
      (value.magic === 'ZLAP' || value.magic === 'ZLAR')
      && value.version === 1
      && typeof value.base === 'string'
    )
      ? 'base'
      : typeof value.applicationPayload === 'string'
        ? 'applicationPayload'
        : undefined;
    if (nestedKey !== undefined) {
      value[nestedKey] = rewrite(
        Buffer.from(value[nestedKey] as string, 'base64'),
        depth + 1
      ).toString('base64');
      return Buffer.from(JSON.stringify(value));
    }
    if (
      value.version !== ACTOR_AUTHORITY_VERSION
      || typeof value.ownerId !== 'string'
      || typeof value.ownerLeaseGeneration !== 'string'
    ) {
      throw new Error('Actor authority payload identity is invalid.');
    }
    value.ownerId = owner.ownerId;
    value.ownerLeaseGeneration = owner.leaseGeneration.toString();
    return Buffer.from(JSON.stringify(value));
  };
  return rewrite(Buffer.from(payload), 0);
}

function requireActorAuthority(
  snapshot: ZLinkAuthoritySnapshot,
  expected: ZLinkActorAuthorityIdentity
): void {
  const actual = decodeActorAuthorityIdentity(snapshot.payload);
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
