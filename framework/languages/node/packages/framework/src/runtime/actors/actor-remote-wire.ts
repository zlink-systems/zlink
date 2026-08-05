import type { RoutingId, SpotId, ZLinkActorJoinOperationId } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef } from '../backend/contracts';
import type { ZLinkRemoteBoundSessionTarget } from './actor-runtime-state';
import type { ZLinkActorHandoffPacket, ZLinkActorHandoffResult } from './actor-handoff';
import { decodeRoutingId, routingIdWireHex } from '../routing-id';

export const ZLINK_REMOTE_ACTOR_JOIN_PACKET = '__zlink.actor.join_spot.request';
export const ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL =
  '__zlink.actor.source_leave.terminal';
export const REMOTE_ACTOR_JOIN_PACKET = ZLINK_REMOTE_ACTOR_JOIN_PACKET;
export const REMOTE_ACTOR_JOIN_ADMISSION = 'admission';
export const REMOTE_ACTOR_JOIN_COMMIT = 'commit';
export const REMOTE_ACTOR_JOIN_ABORT = 'abort';
export type ZLinkRemoteActorJoinPhase =
  | typeof REMOTE_ACTOR_JOIN_ADMISSION
  | typeof REMOTE_ACTOR_JOIN_COMMIT
  | typeof REMOTE_ACTOR_JOIN_ABORT;

export interface ZLinkRemoteActorSourceLeaveTerminal {
  readonly packetName: typeof ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL;
  readonly transferId: string;
  readonly actorId: string;
  readonly succeeded: boolean;
}

export function decodeRemoteActorSourceLeaveTerminal(
  payload: Buffer
): ZLinkRemoteActorSourceLeaveTerminal | undefined {
  let decoded: unknown;
  try {
    decoded = JSON.parse(payload.toString());
  } catch {
    return undefined;
  }
  if (
    typeof decoded !== 'object'
    || decoded === null
    || (decoded as { packetName?: unknown }).packetName
      !== ZLINK_REMOTE_ACTOR_SOURCE_LEAVE_TERMINAL
    || typeof (decoded as { transferId?: unknown }).transferId !== 'string'
    || typeof (decoded as { actorId?: unknown }).actorId !== 'string'
    || typeof (decoded as { succeeded?: unknown }).succeeded !== 'boolean'
  ) {
    return undefined;
  }
  return decoded as ZLinkRemoteActorSourceLeaveTerminal;
}
export interface ZLinkRemoteActorJoinWirePayload {
  readonly packetName?: unknown;
  readonly spotId?: unknown;
  readonly actorId?: unknown;
  readonly actorType?: unknown;
  readonly actorNodeRid?: unknown;
  readonly actorNodeRidHex?: unknown;
  readonly actorGeneration?: unknown;
  readonly expectedMembershipEpoch?: unknown;
  readonly actorEntryNodeRid?: unknown;
  readonly actorEntryNodeRidHex?: unknown;
  readonly actorCreateRequest?: unknown;
  readonly phase?: unknown;
  readonly transferId?: unknown;
  readonly transferAdapterKey?: unknown;
  readonly transferState?: unknown;
  readonly transferStateReference?: unknown;
  readonly transferStateChecksumCrc32c?: unknown;
  readonly sourceSpotId?: unknown;
  readonly routerChannelId?: unknown;
  readonly boundSessionRouterChannelId?: unknown;
  readonly boundSessionTargetNodeRid?: unknown;
  readonly boundSessionTargetNodeRidHex?: unknown;
  readonly boundSessionSpotId?: unknown;
  readonly boundSessionNodeRid?: unknown;
  readonly boundSessionNodeRidHex?: unknown;
  readonly boundSessionRid?: unknown;
  readonly boundSessionRidHex?: unknown;
  readonly boundSessionBindingGeneration?: unknown;
  readonly boundSessionPreviousAuthorityOwnerGeneration?: unknown;
  readonly boundSessionPreviousOwnerLeaseGeneration?: unknown;
  readonly boundSessionAcceptedHighWater?: unknown;
  readonly boundSessionRelocationSealId?: unknown;
  readonly boundSessionAcceptedJournalReference?: unknown;
  readonly boundSessionAcceptedJournalChecksumCrc32c?: unknown;
  readonly request?: unknown;
  readonly handoffBacklog?: unknown;
  readonly completionOperationHigh?: unknown;
  readonly completionOperationLow?: unknown;
}

export interface ZLinkRemoteActorJoinRequest {
  readonly packetName: typeof REMOTE_ACTOR_JOIN_PACKET;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorNodeRid: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration: string;
  readonly expectedMembershipEpoch: string;
  readonly actorEntryNodeRid?: string;
  readonly actorEntryNodeRidHex?: string;
  readonly actorCreateRequest?: string;
  readonly phase?: ZLinkRemoteActorJoinPhase;
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: string;
  readonly transferStateReference?: string;
  readonly transferStateChecksumCrc32c?: number;
  readonly routerChannelId?: string;
  readonly sourceSpotId?: string;
  readonly boundSessionRouterChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionTargetNodeRidHex?: string;
  readonly boundSessionSpotId?: string;
  readonly boundSessionNodeRid?: string;
  readonly boundSessionNodeRidHex?: string;
  readonly boundSessionRid?: string;
  readonly boundSessionRidHex?: string;
  readonly boundSessionBindingGeneration?: string;
  readonly boundSessionPreviousAuthorityOwnerGeneration?: string;
  readonly boundSessionPreviousOwnerLeaseGeneration?: string;
  readonly boundSessionAcceptedHighWater?: string;
  readonly boundSessionRelocationSealId?: string;
  readonly boundSessionAcceptedJournalReference?: string;
  readonly boundSessionAcceptedJournalChecksumCrc32c?: number;
  readonly handoffBacklog?: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationHigh?: string;
  readonly completionOperationLow?: string;
}

export interface ZLinkRemoteActorJoinRequestPayload {
  readonly packetName: typeof REMOTE_ACTOR_JOIN_PACKET;
  readonly spotId?: string;
  readonly actorId?: string;
  readonly actorType: string;
  readonly actorNodeRid?: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration?: string;
  readonly expectedMembershipEpoch?: string;
  readonly actorEntryNodeRid?: string;
  readonly actorEntryNodeRidHex?: string;
  readonly actorCreateRequest?: string;
  readonly phase?: ZLinkRemoteActorJoinPhase;
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: string;
  readonly transferStateReference?: string;
  readonly transferStateChecksumCrc32c?: number;
  readonly sourceSpotId?: string;
  readonly routerChannelId?: string;
  readonly boundSessionRouterChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionTargetNodeRidHex?: string;
  readonly boundSessionSpotId?: string;
  readonly boundSessionNodeRid?: string;
  readonly boundSessionNodeRidHex?: string;
  readonly boundSessionRid?: string;
  readonly boundSessionRidHex?: string;
  readonly boundSessionBindingGeneration?: string;
  readonly boundSessionPreviousAuthorityOwnerGeneration?: string;
  readonly boundSessionPreviousOwnerLeaseGeneration?: string;
  readonly boundSessionAcceptedHighWater?: string;
  readonly boundSessionRelocationSealId?: string;
  readonly boundSessionAcceptedJournalReference?: string;
  readonly boundSessionAcceptedJournalChecksumCrc32c?: number;
  readonly request?: string;
  readonly handoffBacklog?: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationHigh?: string;
  readonly completionOperationLow?: string;
}

interface ZLinkRemoteActorJoinRequestPayloadOptions {
  readonly actorId?: string;
  readonly actorType: string;
  readonly actorRef?: ZLinkBackendActorRef;
  readonly expectedMembershipEpoch?: bigint;
  readonly actorEntryNodeRid?: RoutingId;
  readonly actorCreateRequest?: Buffer;
  readonly request?: Message;
  readonly targetSpotId?: SpotId;
  readonly routerChannelId?: string;
  readonly sourceSpotId?: SpotId;
  readonly boundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly phase?: ZLinkRemoteActorJoinPhase;
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: Buffer;
  readonly transferStateReference?: string;
  readonly transferStateChecksumCrc32c?: number;
  readonly handoffBacklog?: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationId?: ZLinkActorJoinOperationId;
}

export interface ZLinkRemoteActorJoinReply {
  readonly accepted: boolean;
  readonly actorNodeRid: string;
  readonly actorNodeRidHex?: string;
  readonly actorId: string;
  readonly actorGeneration: string;
  readonly handoffResults?: readonly ZLinkActorHandoffResult[];
}

export function buildRemoteActorJoinRequestPayload(
  options: ZLinkRemoteActorJoinRequestPayloadOptions
): ZLinkRemoteActorJoinRequestPayload {
  const actorRef = options.actorRef;
  const sourceSpotId = options.sourceSpotId;
  const boundSessionTarget = options.boundSessionTarget;
  return {
    packetName: REMOTE_ACTOR_JOIN_PACKET,
    spotId: options.targetSpotId === undefined ? undefined : String(options.targetSpotId),
    actorId: options.actorId,
    actorType: options.actorType,
    actorNodeRid: actorRef === undefined ? undefined : String(actorRef.nodeRid),
    actorNodeRidHex: actorRef === undefined ? undefined : encodeRoutingIdHex(actorRef.nodeRid),
    actorGeneration: actorRef === undefined ? undefined : actorRef.generation.toString(),
    expectedMembershipEpoch: options.expectedMembershipEpoch?.toString(),
    actorEntryNodeRid: options.actorEntryNodeRid === undefined ? undefined : String(options.actorEntryNodeRid),
    actorEntryNodeRidHex: options.actorEntryNodeRid === undefined ? undefined : encodeRoutingIdHex(options.actorEntryNodeRid),
    actorCreateRequest: options.actorCreateRequest?.toString('base64'),
    phase: options.phase,
    transferId: options.transferId,
    transferAdapterKey: options.transferAdapterKey,
    transferState: options.transferState?.toString('base64'),
    transferStateReference: options.transferStateReference,
    transferStateChecksumCrc32c: options.transferStateChecksumCrc32c,
    handoffBacklog: options.handoffBacklog,
    completionOperationHigh: options.completionOperationId?.high.toString(),
    completionOperationLow: options.completionOperationId?.low.toString(),
    sourceSpotId: sourceSpotId === undefined ? undefined : String(sourceSpotId),
    routerChannelId: options.routerChannelId,
    boundSessionRouterChannelId: boundSessionTarget?.routerChannelId,
    boundSessionTargetNodeRid: boundSessionTarget === undefined ? undefined : String(boundSessionTarget.targetNodeRid),
    boundSessionTargetNodeRidHex: boundSessionTarget === undefined ? undefined : encodeRoutingIdHex(boundSessionTarget.targetNodeRid),
    boundSessionSpotId: boundSessionTarget === undefined ? undefined : String(boundSessionTarget.spotId),
    boundSessionNodeRid: boundSessionTarget?.sessionNodeRid === undefined
      ? undefined
      : String(boundSessionTarget.sessionNodeRid),
    boundSessionNodeRidHex: boundSessionTarget?.sessionNodeRid === undefined
      ? undefined
      : encodeRoutingIdHex(boundSessionTarget.sessionNodeRid),
    boundSessionRid: boundSessionTarget?.sessionRid === undefined
      ? undefined
      : String(boundSessionTarget.sessionRid),
    boundSessionRidHex: boundSessionTarget?.sessionRid === undefined
      ? undefined
      : encodeRoutingIdHex(boundSessionTarget.sessionRid),
    boundSessionBindingGeneration: boundSessionTarget?.bindingGeneration?.toString(),
    boundSessionPreviousAuthorityOwnerGeneration:
      boundSessionTarget?.previousAuthorityOwnerGeneration?.toString(),
    boundSessionPreviousOwnerLeaseGeneration:
      boundSessionTarget?.previousOwnerLeaseGeneration?.toString(),
    boundSessionAcceptedHighWater: boundSessionTarget?.acceptedHighWater?.toString(),
    boundSessionRelocationSealId: boundSessionTarget?.relocationSealId,
    boundSessionAcceptedJournalReference: boundSessionTarget?.acceptedJournalReference,
    boundSessionAcceptedJournalChecksumCrc32c: boundSessionTarget?.acceptedJournalChecksumCrc32c,
    request: options.request === undefined ? undefined : options.request.data().toString('base64')
  };
}

export function decodeRemoteActorJoinPayload(payload: unknown): {
  readonly spotId: SpotId;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorNodeRid: RoutingId;
  readonly actorGeneration: string;
  readonly sourceSpotId?: SpotId;
  readonly routerChannelId?: string;
  readonly boundSessionRouterChannelId?: string;
  readonly boundSessionTargetNodeRid?: RoutingId;
  readonly boundSessionSpotId?: SpotId;
  readonly request: string;
} {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    (payload as { packetName?: unknown }).packetName !== ZLINK_REMOTE_ACTOR_JOIN_PACKET ||
    typeof (payload as { spotId?: unknown }).spotId !== 'string' ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { actorType?: unknown }).actorType !== 'string' ||
    typeof (payload as { actorNodeRid?: unknown }).actorNodeRid !== 'string' ||
    typeof (payload as { actorGeneration?: unknown }).actorGeneration !== 'string' ||
    typeof (payload as { request?: unknown }).request !== 'string'
  ) {
    throw new Error('Remote actor join payload is invalid.');
  }
  return {
    spotId: requireSpotId((payload as { spotId: string }).spotId),
    actorId: (payload as { actorId: string }).actorId,
    actorType: (payload as { actorType: string }).actorType,
    actorNodeRid: decodeWireRoutingId(
      (payload as { actorNodeRid: string }).actorNodeRid,
      optionalString(payload, 'actorNodeRidHex')
    ),
    actorGeneration: (payload as { actorGeneration: string }).actorGeneration,
    sourceSpotId: optionalSpotId(payload, 'sourceSpotId'),
    routerChannelId: optionalString(payload, 'routerChannelId'),
    boundSessionRouterChannelId: optionalString(payload, 'boundSessionRouterChannelId'),
    boundSessionTargetNodeRid: optionalRoutingId(
      payload,
      'boundSessionTargetNodeRid',
      'boundSessionTargetNodeRidHex'
    ),
    boundSessionSpotId: optionalSpotId(payload, 'boundSessionSpotId'),
    request: (payload as { request: string }).request
  };
}

function encodeRoutingIdHex(routingId: RoutingId): string | undefined {
  return routingIdWireHex(routingId);
}

export function decodeWireRoutingId(text: string, hex: string | undefined): RoutingId {
  return decodeRoutingId(text, hex);
}

function optionalString(value: object, key: string): string | undefined {
  const field = (value as Record<string, unknown>)[key];
  return typeof field === 'string' ? field : undefined;
}

function optionalRoutingId(value: object, textKey: string, hexKey: string): RoutingId | undefined {
  const text = optionalString(value, textKey);
  return text === undefined ? undefined : decodeWireRoutingId(text, optionalString(value, hexKey));
}

function optionalSpotId(value: object, key: string): SpotId | undefined {
  const text = optionalString(value, key);
  return text === undefined ? undefined : requireSpotId(text);
}

function requireSpotId(value: string): SpotId {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 255) {
    throw new Error('Remote actor SpotId must contain 1..255 UTF-8 bytes.');
  }
  return value;
}
