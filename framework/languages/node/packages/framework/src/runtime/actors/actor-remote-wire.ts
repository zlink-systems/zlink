import type { RoutingId, SpotId, ZLinkActorJoinOperationId } from '../../contracts';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkBackendActorRef } from '../backend/contracts';
import type { ZLinkRemoteBoundSessionTarget } from './actor-runtime-state';
import type { ZLinkActorHandoffPacket } from './actor-handoff';
import { decodeRoutingId, routingIdWireHex } from '../routing-id';
import { frameworkPayloadContentType } from '../messaging/payload-codec';

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
  readonly actorNodeGeneration?: unknown;
  readonly expectedAuthorityOwnerGeneration?: unknown;
  readonly expectedOwnerLeaseGeneration?: unknown;
  readonly expectedMembershipEpoch?: unknown;
  readonly actorEntryNodeRid?: unknown;
  readonly actorEntryNodeRidHex?: unknown;
  readonly actorCreateRequest?: unknown;
  readonly phase?: unknown;
  readonly transferId?: unknown;
  readonly transferAdapterKey?: unknown;
  readonly transferState?: unknown;
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
  readonly boundSessionRelocationSealId?: unknown;
  readonly boundSessionServiceWireRelocation?: unknown;
  readonly request?: unknown;
  readonly requestContentType?: unknown;
  readonly handoffBacklog?: unknown;
  readonly completionOperationHigh?: unknown;
  readonly completionOperationLow?: unknown;
}

export interface ZLinkRemoteActorJoinRequestPayload {
  readonly packetName: typeof REMOTE_ACTOR_JOIN_PACKET;
  readonly spotId?: string;
  readonly actorId?: string;
  readonly actorType: string;
  readonly actorNodeRid?: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration?: string;
  /** Current owner MeshNode lifecycle generation for the Actor authority fence. */
  readonly actorNodeGeneration?: string;
  /** Current Actor Authority owner-generation fence. */
  readonly expectedAuthorityOwnerGeneration?: string;
  /** Current Actor Authority owner-lease fence. */
  readonly expectedOwnerLeaseGeneration?: string;
  readonly expectedMembershipEpoch?: string;
  readonly actorEntryNodeRid?: string;
  readonly actorEntryNodeRidHex?: string;
  readonly actorCreateRequest?: string;
  readonly phase?: ZLinkRemoteActorJoinPhase;
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: string;
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
  readonly boundSessionRelocationSealId?: string;
  readonly boundSessionServiceWireRelocation?: {
    readonly relocationHigh: string;
    readonly relocationLow: string;
    readonly coordinatorOwnerId: string;
    readonly coordinatorLeaseGeneration: string;
    readonly coordinatorNodeRid: string;
    readonly coordinatorNodeGeneration: string;
    readonly coordinatorExpectedAuthorityStoreVersion: string;
    readonly sessionOwnerNodeRid: string;
    readonly sessionOwnerNodeGeneration: string;
    readonly sessionOwnerId: string;
    readonly sessionOwnerLeaseGeneration: string;
    readonly sessionRid: string;
    readonly bindingGeneration: string;
  };
  readonly request?: string;
  readonly requestContentType?: string;
  readonly handoffBacklog?: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationHigh?: string;
  readonly completionOperationLow?: string;
}

interface ZLinkRemoteActorJoinRequestPayloadOptions {
  readonly actorId?: string;
  readonly actorType: string;
  readonly actorRef?: ZLinkBackendActorRef;
  /**
   * The legacy JSON transport retains its shape for now, but remote admission
   * still needs the complete Authority fence to verify the Store row.
   */
  readonly actorAuthorityFence?: {
    readonly nodeGeneration: bigint;
    readonly authorityOwnerGeneration: bigint;
    readonly ownerLeaseGeneration: bigint;
  };
  readonly expectedMembershipEpoch?: bigint;
  readonly actorEntryNodeRid?: RoutingId;
  readonly actorCreateRequest?: Buffer;
  readonly request?: Message;
  readonly requestContentType?: string;
  readonly targetSpotId?: SpotId;
  readonly routerChannelId?: string;
  readonly sourceSpotId?: SpotId;
  readonly boundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly phase?: ZLinkRemoteActorJoinPhase;
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: Buffer;
  readonly handoffBacklog?: readonly ZLinkActorHandoffPacket[];
  readonly completionOperationId?: ZLinkActorJoinOperationId;
}

export function buildRemoteActorJoinRequestPayload(
  options: ZLinkRemoteActorJoinRequestPayloadOptions
): ZLinkRemoteActorJoinRequestPayload {
  const actorRef = options.actorRef;
  const actorAuthorityFence = options.actorAuthorityFence;
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
    actorNodeGeneration: actorAuthorityFence?.nodeGeneration.toString(),
    expectedAuthorityOwnerGeneration: actorAuthorityFence?.authorityOwnerGeneration.toString(),
    expectedOwnerLeaseGeneration: actorAuthorityFence?.ownerLeaseGeneration.toString(),
    expectedMembershipEpoch: options.expectedMembershipEpoch?.toString(),
    actorEntryNodeRid: options.actorEntryNodeRid === undefined ? undefined : String(options.actorEntryNodeRid),
    actorEntryNodeRidHex: options.actorEntryNodeRid === undefined ? undefined : encodeRoutingIdHex(options.actorEntryNodeRid),
    actorCreateRequest: options.actorCreateRequest?.toString('base64'),
    phase: options.phase,
    transferId: options.transferId,
    transferAdapterKey: options.transferAdapterKey,
    transferState: options.transferState?.toString('base64'),
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
    boundSessionRelocationSealId: boundSessionTarget?.relocationSealId,
    boundSessionServiceWireRelocation: boundSessionTarget?.serviceWireRelocation === undefined
      ? undefined
      : {
          relocationHigh: boundSessionTarget.serviceWireRelocation.relocation.high.toString(),
          relocationLow: boundSessionTarget.serviceWireRelocation.relocation.low.toString(),
          coordinatorOwnerId: boundSessionTarget.serviceWireRelocation.coordinator.ownerId,
          coordinatorLeaseGeneration:
            boundSessionTarget.serviceWireRelocation.coordinator.leaseGeneration.toString(),
          coordinatorNodeRid: boundSessionTarget.serviceWireRelocation.coordinator.nodeRid,
          coordinatorNodeGeneration:
            boundSessionTarget.serviceWireRelocation.coordinator.nodeGeneration.toString(),
          coordinatorExpectedAuthorityStoreVersion:
            boundSessionTarget.serviceWireRelocation.coordinator.expectedAuthorityStoreVersion,
          sessionOwnerNodeRid: boundSessionTarget.serviceWireRelocation.session.sessionOwnerNodeRid,
          sessionOwnerNodeGeneration:
            boundSessionTarget.serviceWireRelocation.session.sessionOwnerNodeGeneration.toString(),
          sessionOwnerId: boundSessionTarget.serviceWireRelocation.session.sessionOwnerId,
          sessionOwnerLeaseGeneration:
            boundSessionTarget.serviceWireRelocation.session.sessionOwnerLeaseGeneration.toString(),
          sessionRid: boundSessionTarget.serviceWireRelocation.session.sessionRid,
          bindingGeneration:
            boundSessionTarget.serviceWireRelocation.session.bindingGeneration.toString()
        },
    request: options.request === undefined ? undefined : options.request.data().toString('base64'),
    requestContentType: options.request === undefined
      ? undefined
      : options.requestContentType ?? frameworkPayloadContentType(options.request)
  };
}

export function decodeRemoteActorJoinPayload(payload: unknown): {
  readonly spotId: SpotId;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorNodeRid: RoutingId;
  readonly actorGeneration: string;
  readonly actorNodeGeneration: string;
  readonly expectedAuthorityOwnerGeneration: string;
  readonly expectedOwnerLeaseGeneration: string;
  readonly sourceSpotId?: SpotId;
  readonly routerChannelId?: string;
  readonly boundSessionRouterChannelId?: string;
  readonly boundSessionTargetNodeRid?: RoutingId;
  readonly boundSessionSpotId?: SpotId;
  readonly request: string;
  readonly requestContentType: string;
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
    typeof (payload as { actorNodeGeneration?: unknown }).actorNodeGeneration !== 'string' ||
    typeof (payload as { expectedAuthorityOwnerGeneration?: unknown }).expectedAuthorityOwnerGeneration !== 'string' ||
    typeof (payload as { expectedOwnerLeaseGeneration?: unknown }).expectedOwnerLeaseGeneration !== 'string' ||
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
    actorNodeGeneration: (payload as { actorNodeGeneration: string }).actorNodeGeneration,
    expectedAuthorityOwnerGeneration:
      (payload as { expectedAuthorityOwnerGeneration: string }).expectedAuthorityOwnerGeneration,
    expectedOwnerLeaseGeneration:
      (payload as { expectedOwnerLeaseGeneration: string }).expectedOwnerLeaseGeneration,
    sourceSpotId: optionalSpotId(payload, 'sourceSpotId'),
    routerChannelId: optionalString(payload, 'routerChannelId'),
    boundSessionRouterChannelId: optionalString(payload, 'boundSessionRouterChannelId'),
    boundSessionTargetNodeRid: optionalRoutingId(
      payload,
      'boundSessionTargetNodeRid',
      'boundSessionTargetNodeRidHex'
    ),
    boundSessionSpotId: optionalSpotId(payload, 'boundSessionSpotId'),
    request: (payload as { request: string }).request,
    requestContentType: optionalString(payload, 'requestContentType') ?? 'application/json'
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
