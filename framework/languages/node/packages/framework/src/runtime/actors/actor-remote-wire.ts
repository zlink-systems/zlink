import type { RoutingId, SpotId } from '../../contracts';
import { decodeRoutingId } from '../routing-id';

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
