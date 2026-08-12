import { createHash, randomBytes } from 'node:crypto';
import type { ActorRef } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkResolvedActorRoute } from '../locations';
import { decodeRoutingId, encodeRoutingIdStorageHex } from '../routing-id';

export const ZLINK_MESSAGE_FOLLOW_MAX_HOPS = 8;

export interface ZLinkActorMessageFollowOwnerFence {
  readonly ownerId: string;
  readonly ownerLeaseGeneration: string;
  readonly nodeRid: string;
  /** Canonical opaque RoutingId bytes. Older wire contexts may omit this field. */
  readonly nodeRidHex?: string;
  readonly nodeGeneration: string;
  readonly authorityOwnerGeneration: string;
}

export interface ZLinkActorMessageFollowContext {
  readonly operationId: string;
  readonly objectGeneration: string;
  readonly sourceOwner: ZLinkActorMessageFollowOwnerFence;
  readonly targetOwner: ZLinkActorMessageFollowOwnerFence;
  readonly deadlineUnixMs?: number;
  readonly correlationId?: string;
  readonly replyRouteId?: string;
  readonly request: boolean;
  readonly hopCount: number;
  readonly visitedOwners: readonly string[];
  readonly payloadChecksumSha256: string;
}

type ActorRefWithMessageFollow = ActorRef & {
  readonly __zlinkMessageFollowContext?: ZLinkActorMessageFollowContext;
};

export function createMessageFollowId(): string {
  return randomBytes(16).toString('hex');
}

export function createInitialActorMessageFollowContext(
  route: ZLinkResolvedActorRoute,
  parts: readonly Message[],
  request: boolean,
  deadlineUnixMs?: number,
  correlationId?: string,
  operationId = createMessageFollowId(),
  replyRouteId = request ? createMessageFollowId() : undefined
): ZLinkActorMessageFollowContext {
  const owner = ownerFence({
    ownerId: route.ownerId,
    ownerLeaseGeneration: route.ownerLeaseGeneration,
    nodeRid: String(route.actorRef.nodeRid),
    nodeRidHex: encodeRoutingIdStorageHex(route.actorRef.nodeRid),
    nodeGeneration: route.ownerNodeGeneration,
    authorityOwnerGeneration: route.authorityOwnerGeneration
  });
  return freezeContext({
    operationId,
    objectGeneration: route.actorRef.objectGeneration.toString(),
    sourceOwner: owner,
    targetOwner: owner,
    deadlineUnixMs,
    correlationId,
    replyRouteId,
    request,
    hopCount: 0,
    visitedOwners: [messageFollowOwnerFenceKey(owner)],
    payloadChecksumSha256: actorMessageFollowPayloadChecksum(parts)
  });
}

export function decodeActorMessageFollowContext(
  value: unknown
): ZLinkActorMessageFollowContext | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== 'object' || value === null) {
    throw invalidContext('context must be an object');
  }
  const input = value as Record<string, unknown>;
  const operationId = requireId(input.operationId, 'operationId');
  const objectGeneration = requirePositiveBigInt(input.objectGeneration, 'objectGeneration');
  const sourceOwner = decodeOwnerFence(input.sourceOwner, 'sourceOwner');
  const targetOwner = decodeOwnerFence(input.targetOwner, 'targetOwner');
  const request = input.request;
  const hopCount = input.hopCount;
  const checksum = input.payloadChecksumSha256;
  if (typeof request !== 'boolean') throw invalidContext('request must be boolean');
  if (!Number.isSafeInteger(hopCount) || (hopCount as number) < 0
      || (hopCount as number) > ZLINK_MESSAGE_FOLLOW_MAX_HOPS) {
    throw invalidContext('hopCount is outside 0..8');
  }
  if (typeof checksum !== 'string' || !/^[0-9a-f]{64}$/.test(checksum)) {
    throw invalidContext('payload checksum is invalid');
  }
  const deadlineUnixMs = optionalPositiveSafeInteger(input.deadlineUnixMs, 'deadlineUnixMs');
  const correlationId = optionalId(input.correlationId, 'correlationId');
  const replyRouteId = optionalId(input.replyRouteId, 'replyRouteId');
  if (request && (correlationId === undefined || replyRouteId === undefined)) {
    throw invalidContext('request context requires correlationId and replyRouteId');
  }
  if (!request && replyRouteId !== undefined) {
    throw invalidContext('one-way context must not contain replyRouteId');
  }
  if (!Array.isArray(input.visitedOwners)
      || input.visitedOwners.some(item => typeof item !== 'string' || item.length === 0)
      || input.visitedOwners.length !== (hopCount as number) + 1) {
    throw invalidContext('visitedOwners does not match hopCount');
  }
  const visitedOwners = [...input.visitedOwners] as string[];
  const targetOwnerKeys = messageFollowOwnerFenceCompatibleKeys(targetOwner);
  if (new Set(visitedOwners).size !== visitedOwners.length
      || !targetOwnerKeys.includes(visitedOwners[visitedOwners.length - 1])
      || visitedOwners.slice(0, -1).some(key => targetOwnerKeys.includes(key))) {
    throw invalidContext('visited owner fence chain is invalid');
  }
  return freezeContext({
    operationId,
    objectGeneration,
    sourceOwner,
    targetOwner,
    deadlineUnixMs,
    correlationId,
    replyRouteId,
    request,
    hopCount: hopCount as number,
    visitedOwners,
    payloadChecksumSha256: checksum
  });
}

export function advanceActorMessageFollowContext(
  context: ZLinkActorMessageFollowContext,
  sourceOwner: ZLinkActorMessageFollowOwnerFence,
  targetOwner: ZLinkActorMessageFollowOwnerFence
): ZLinkActorMessageFollowContext {
  if (!messageFollowOwnerFencesEqual(context.targetOwner, sourceOwner)) {
    throw invalidContext('source owner fence does not match the routed owner');
  }
  if (context.hopCount >= ZLINK_MESSAGE_FOLLOW_MAX_HOPS) {
    throw invalidContext('Message Follow reached the 8-hop limit');
  }
  const targetKey = messageFollowOwnerFenceKey(targetOwner);
  if (messageFollowOwnerFenceCompatibleKeys(targetOwner)
    .some(key => context.visitedOwners.includes(key))) {
    throw invalidContext('Message Follow owner loop was detected');
  }
  return freezeContext({
    ...context,
    sourceOwner,
    targetOwner,
    hopCount: context.hopCount + 1,
    visitedOwners: [...context.visitedOwners, targetKey]
  });
}

export function attachActorMessageFollowContext(
  actorRef: ActorRef,
  context: ZLinkActorMessageFollowContext
): ActorRef {
  return Object.freeze({
    ...actorRef,
    __zlinkMessageFollowContext: context
  }) as ActorRef;
}

export function actorMessageFollowContext(
  actorRef: ActorRef | undefined
): ZLinkActorMessageFollowContext | undefined {
  return (actorRef as ActorRefWithMessageFollow | undefined)
    ?.__zlinkMessageFollowContext;
}

export function actorMessageFollowPayloadChecksum(parts: readonly Message[]): string {
  const hash = createHash('sha256');
  for (const part of parts) {
    const bytes = Buffer.from(part.data());
    const length = Buffer.allocUnsafe(4);
    length.writeUInt32BE(bytes.length);
    hash.update(length);
    hash.update(bytes);
  }
  return hash.digest('hex');
}

export function verifyActorMessageFollowPayload(
  context: ZLinkActorMessageFollowContext,
  parts: readonly Message[]
): void {
  if (actorMessageFollowPayloadChecksum(parts) !== context.payloadChecksumSha256) {
    throw invalidContext('payload checksum does not match');
  }
}

export function messageFollowOwnerFenceKey(
  fence: ZLinkActorMessageFollowOwnerFence
): string {
  return JSON.stringify([
    'zlink-actor-message-follow-owner-fence',
    2,
    fence.nodeRidHex ?? encodeRoutingIdStorageHex(fence.nodeRid),
    fence.nodeGeneration,
    fence.ownerId,
    fence.ownerLeaseGeneration,
    fence.authorityOwnerGeneration
  ]);
}

export function messageFollowOwnerFencesEqual(
  left: ZLinkActorMessageFollowOwnerFence,
  right: ZLinkActorMessageFollowOwnerFence
): boolean {
  return messageFollowOwnerFenceKey(left) === messageFollowOwnerFenceKey(right);
}

export function messageFollowOwnerNodeRid(
  fence: ZLinkActorMessageFollowOwnerFence
): ReturnType<typeof decodeRoutingId> {
  return decodeRoutingId(fence.nodeRid, fence.nodeRidHex);
}

export function ownerFence(input: {
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint | string;
  readonly nodeRid: string;
  readonly nodeRidHex?: string;
  readonly nodeGeneration: bigint | string;
  readonly authorityOwnerGeneration: bigint | string;
}): ZLinkActorMessageFollowOwnerFence {
  const nodeRidHex = optionalNodeRidHex(input.nodeRidHex, 'nodeRidHex');
  const value = {
    ownerId: requireText(input.ownerId, 'ownerId'),
    ownerLeaseGeneration: requirePositiveBigInt(
      String(input.ownerLeaseGeneration),
      'ownerLeaseGeneration'
    ),
    nodeRid: requireText(input.nodeRid, 'nodeRid'),
    ...(nodeRidHex === undefined ? {} : { nodeRidHex }),
    nodeGeneration: requirePositiveBigInt(String(input.nodeGeneration), 'nodeGeneration'),
    authorityOwnerGeneration: requirePositiveBigInt(
      String(input.authorityOwnerGeneration),
      'authorityOwnerGeneration'
    )
  };
  return Object.freeze(value);
}

function decodeOwnerFence(
  value: unknown,
  name: string
): ZLinkActorMessageFollowOwnerFence {
  if (typeof value !== 'object' || value === null) {
    throw invalidContext(`${name} must be an object`);
  }
  const input = value as Record<string, unknown>;
  return ownerFence({
    ownerId: requireText(input.ownerId, `${name}.ownerId`),
    ownerLeaseGeneration: requirePositiveBigInt(
      input.ownerLeaseGeneration,
      `${name}.ownerLeaseGeneration`
    ),
    nodeRid: requireText(input.nodeRid, `${name}.nodeRid`),
    nodeRidHex: optionalNodeRidHex(input.nodeRidHex, `${name}.nodeRidHex`),
    nodeGeneration: requirePositiveBigInt(input.nodeGeneration, `${name}.nodeGeneration`),
    authorityOwnerGeneration: requirePositiveBigInt(
      input.authorityOwnerGeneration,
      `${name}.authorityOwnerGeneration`
    )
  });
}

function freezeContext(
  context: ZLinkActorMessageFollowContext
): ZLinkActorMessageFollowContext {
  return Object.freeze({
    ...context,
    sourceOwner: Object.freeze({ ...context.sourceOwner }),
    targetOwner: Object.freeze({ ...context.targetOwner }),
    visitedOwners: Object.freeze([...context.visitedOwners])
  });
}

function requireId(value: unknown, name: string): string {
  if (typeof value !== 'string' || !/^[0-9a-f]{32}$/.test(value)
      || /^0+$/.test(value)) {
    throw invalidContext(`${name} must be a nonzero 128-bit lowercase hex value`);
  }
  return value;
}

function optionalId(value: unknown, name: string): string | undefined {
  return value === undefined ? undefined : requireId(value, name);
}

function requireText(value: unknown, name: string): string {
  if (typeof value !== 'string' || value.length === 0) {
    throw invalidContext(`${name} must be a non-empty string`);
  }
  return value;
}

function requirePositiveBigInt(value: unknown, name: string): string {
  if (typeof value !== 'string' || !/^[1-9][0-9]*$/.test(value)) {
    throw invalidContext(`${name} must be a positive decimal integer`);
  }
  return value;
}

function optionalNodeRidHex(value: unknown, name: string): string | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== 'string' || value.length === 0 || value.length % 2 !== 0
      || !/^[0-9a-f]+$/.test(value)) {
    throw invalidContext(`${name} must be non-empty lowercase hexadecimal bytes`);
  }
  return value;
}

function messageFollowOwnerFenceCompatibleKeys(
  fence: ZLinkActorMessageFollowOwnerFence
): readonly string[] {
  const canonical = messageFollowOwnerFenceKey(fence);
  const textHex = encodeRoutingIdStorageHex(fence.nodeRid);
  if (fence.nodeRid.includes('\u0000')
      || fence.ownerId.includes('\u0000')
      || (fence.nodeRidHex !== undefined && fence.nodeRidHex !== textHex)) {
    return [canonical];
  }
  return [canonical, [
    fence.nodeRid,
    fence.nodeGeneration,
    fence.ownerId,
    fence.ownerLeaseGeneration,
    fence.authorityOwnerGeneration
  ].join('\u0000')];
}

function optionalPositiveSafeInteger(value: unknown, name: string): number | undefined {
  if (value === undefined) return undefined;
  if (!Number.isSafeInteger(value) || (value as number) <= 0) {
    throw invalidContext(`${name} must be a positive safe integer`);
  }
  return value as number;
}

function invalidContext(reason: string): Error {
  return new Error(`Actor Message Follow context is invalid: ${reason}.`);
}
