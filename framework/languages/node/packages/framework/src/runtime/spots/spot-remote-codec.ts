import type { ZLinkBackendActorRef } from '../backend/contracts';
import {
  decodeActorMessageFollowContext,
  type ZLinkRemoteBoundSessionTarget
} from '../actors';
import type { RoutingId, ZLinkActor } from '../../contracts';
import type { ZLinkActorHandoffPacket } from '../actors/actor-handoff';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import type { ZLinkBackendReceived as BackendReceived } from '../backend/runtime-values';
import { decodeChannelEnvelope } from '../channels/channel-envelope';
import { decodeRoutingId as decodeWireRoutingId } from '../routing-id';
import {
  REMOTE_ACTOR_JOIN_PACKET,
  type ZLinkRemoteActorJoinWirePayload
} from '../actors/actor-remote-wire';

export { REMOTE_ACTOR_JOIN_PACKET };
export const REMOTE_BOUND_SESSION_BIND_PACKET = 'zlink.framework.actor.bound_session.bind';

export interface ZLinkRemoteActorJoinActor {
  readonly actor: ZLinkActor;
  readonly actorRef: ZLinkBackendActorRef;
}

export type ZLinkRoutedActorTransferProvider = (
  actorId: string,
  actorType: string,
  adapterKey: string | undefined,
  transferState: Message,
  actorEntryNodeRid: RoutingId | undefined,
  remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget,
  signal?: AbortSignal
) => Promise<ZLinkRemoteActorJoinActor>;

export interface ZLinkDecodedRemoteActorJoinRequest {
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
  readonly raw: boolean;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorRef?: ZLinkBackendActorRef;
  readonly expectedMembershipEpoch: bigint;
  readonly actorEntryNodeRid?: RoutingId;
  readonly remoteBoundSessionTarget?: ZLinkRemoteBoundSessionTarget;
  readonly actorCreateRequest?: Message;
  readonly request: Message;
  readonly phase?: 'admission' | 'commit';
  readonly transferId?: string;
  readonly transferAdapterKey?: string;
  readonly transferState?: Message;
  readonly handoffBacklog: readonly ZLinkActorHandoffPacket[];
}

export type { ZLinkRemoteActorJoinWirePayload };

export function hasRemoteActorJoinIdentity(
  value: unknown
): value is ZLinkRemoteActorJoinWirePayload & {
  readonly actorId: string;
  readonly actorType: string;
} {
  return (
    typeof value === 'object' &&
    value !== null &&
    typeof (value as { actorId?: unknown }).actorId === 'string' &&
    typeof (value as { actorType?: unknown }).actorType === 'string'
  );
}

export function isRemoteActorJoinPayload(
  value: unknown
): value is ZLinkRemoteActorJoinWirePayload & {
  readonly actorId: string;
  readonly actorType: string;
  readonly request: string;
} {
  return (
    hasRemoteActorJoinIdentity(value) &&
    typeof value.request === 'string'
  );
}

export function decodeRemoteActorJoinPayload(
  payload: ZLinkRemoteActorJoinWirePayload & {
    readonly actorId: string;
    readonly actorType: string;
  },
  request: Message,
  received: BackendReceived,
  raw: boolean,
  envelope?: ReturnType<typeof decodeChannelEnvelope>
): ZLinkDecodedRemoteActorJoinRequest {
  const phase = payload.phase === 'admission' || payload.phase === 'commit'
    ? payload.phase
    : undefined;
  const transferProtocol = phase !== undefined;
  return {
    envelope,
    raw,
    actorId: payload.actorId,
    actorType: payload.actorType,
    actorRef: decodeRemoteActorRef(
      payload.actorNodeRid,
      payload.actorNodeRidHex,
      payload.actorId,
      payload.actorGeneration
    ),
    expectedMembershipEpoch: typeof payload.expectedMembershipEpoch === 'string'
      ? BigInt(payload.expectedMembershipEpoch)
      : 0n,
    actorEntryNodeRid: typeof payload.actorEntryNodeRid === 'string'
      ? decodeWireRoutingId(payload.actorEntryNodeRid, payload.actorEntryNodeRidHex)
      : undefined,
    actorCreateRequest: typeof payload.actorCreateRequest === 'string'
      ? RuntimeMessage.from(Buffer.from(payload.actorCreateRequest, 'base64'))
      : undefined,
    phase,
    transferId: typeof payload.transferId === 'string' ? payload.transferId : undefined,
    transferAdapterKey: typeof payload.transferAdapterKey === 'string'
      ? payload.transferAdapterKey
      : undefined,
    transferState: typeof payload.transferState === 'string'
      ? RuntimeMessage.from(Buffer.from(payload.transferState, 'base64'))
      : undefined,
    handoffBacklog: decodeHandoffBacklog(payload.handoffBacklog),
    remoteBoundSessionTarget: decodeRemoteBoundSessionTarget(
      transferProtocol ? payload.boundSessionRouterChannelId : payload.boundSessionRouterChannelId ?? payload.routerChannelId,
      transferProtocol
        ? payload.boundSessionTargetNodeRid
        : payload.boundSessionTargetNodeRid ?? (
            received.routingId === null
              ? payload.actorNodeRid
              : String(received.routingId)
          ),
      payload.boundSessionTargetNodeRidHex,
      transferProtocol
        ? payload.boundSessionSpotId
        : payload.boundSessionSpotId ?? payload.sourceSpotId,
      payload.boundSessionNodeRid,
      payload.boundSessionNodeRidHex,
      payload.boundSessionRid,
      payload.boundSessionRidHex,
      payload.boundSessionBindingGeneration,
      payload.boundSessionPreviousAuthorityOwnerGeneration,
      payload.boundSessionPreviousOwnerLeaseGeneration,
      payload.boundSessionAcceptedHighWater,
      payload.boundSessionRelocationSealId,
      payload.boundSessionAcceptedJournalReference,
      payload.boundSessionAcceptedJournalChecksumCrc32c
    ),
    request
  };
}

export function decodeHandoffBacklog(value: unknown): readonly ZLinkActorHandoffPacket[] {
  if (!Array.isArray(value)) return [];
  return value.map((entry, expectedIndex) => {
    if (
      typeof entry !== 'object' || entry === null ||
      (entry as { index?: unknown }).index !== expectedIndex ||
      typeof (entry as { header?: unknown }).header !== 'string' ||
      typeof (entry as { payload?: unknown }).payload !== 'string' ||
      typeof (entry as { returnResponse?: unknown }).returnResponse !== 'boolean'
    ) {
      throw new Error('Remote actor handoff backlog is not a contiguous packet sequence.');
    }
    const packet = entry as ZLinkActorHandoffPacket;
    const messageFollowContext = decodeActorMessageFollowContext(
      packet.messageFollowContext
    );
    const source = packet.source;
    if (messageFollowContext === undefined
      || (packet.returnResponse && (
        source === undefined || source.ownerId.length === 0
        || BigInt(source.ownerLeaseGeneration) <= 0n || source.nodeRid.length === 0
        || BigInt(source.nodeGeneration) <= 0n
        || source.replyRouteId !== messageFollowContext.replyRouteId
      ))
      || (!packet.returnResponse && source !== undefined)) {
      throw new Error('Remote actor handoff request source fence is invalid.');
    }
    return { ...packet, messageFollowContext };
  });
}

export function decodeRemoteActorRef(
  nodeRid: unknown,
  nodeRidHex: unknown,
  actorId: string,
  generation: unknown
): ZLinkBackendActorRef | undefined {
  if (typeof nodeRid !== 'string') {
    return undefined;
  }
  return {
    nodeRid: decodeWireRoutingId(nodeRid, nodeRidHex),
    actorId,
    generation: typeof generation === 'string' ? BigInt(generation) : 0n
  } as ZLinkBackendActorRef;
}

export function decodeRemoteBoundSessionTarget(
  routerChannelId: unknown,
  targetNodeRid: unknown,
  targetNodeRidHex: unknown,
  spotId: unknown,
  sessionNodeRid: unknown,
  sessionNodeRidHex: unknown,
  sessionRid: unknown,
  sessionRidHex: unknown,
  bindingGeneration: unknown,
  previousAuthorityOwnerGeneration?: unknown,
  previousOwnerLeaseGeneration?: unknown,
  acceptedHighWater?: unknown,
  relocationSealId?: unknown,
  acceptedJournalReference?: unknown,
  acceptedJournalChecksumCrc32c?: unknown
): ZLinkRemoteBoundSessionTarget | undefined {
  if (
    typeof routerChannelId !== 'string' ||
    typeof targetNodeRid !== 'string' ||
    spotId === undefined ||
    spotId === null
  ) {
    return undefined;
  }
  return {
    routerChannelId,
    targetNodeRid: decodeWireRoutingId(targetNodeRid, targetNodeRidHex),
    spotId: requireSpotId(String(spotId)),
    sessionNodeRid: typeof sessionNodeRid === 'string'
      ? decodeWireRoutingId(sessionNodeRid, sessionNodeRidHex)
      : undefined,
    sessionRid: typeof sessionRid === 'string'
      ? decodeWireRoutingId(sessionRid, sessionRidHex)
      : undefined,
    bindingGeneration: typeof bindingGeneration === 'string'
      ? BigInt(bindingGeneration)
      : undefined,
    previousAuthorityOwnerGeneration: typeof previousAuthorityOwnerGeneration === 'string'
      ? BigInt(previousAuthorityOwnerGeneration)
      : undefined,
    previousOwnerLeaseGeneration: typeof previousOwnerLeaseGeneration === 'string'
      ? BigInt(previousOwnerLeaseGeneration)
      : undefined,
    acceptedHighWater: typeof acceptedHighWater === 'string'
      ? BigInt(acceptedHighWater)
      : undefined,
    relocationSealId: typeof relocationSealId === 'string' ? relocationSealId : undefined,
    acceptedJournalReference: typeof acceptedJournalReference === 'string'
      ? acceptedJournalReference
      : undefined,
    acceptedJournalChecksumCrc32c: typeof acceptedJournalChecksumCrc32c === 'number'
      ? acceptedJournalChecksumCrc32c
      : undefined
  };
}

function requireSpotId(value: string): string {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 255) {
    throw new Error('Remote bound session SpotId must contain 1..255 UTF-8 bytes.');
  }
  return value;
}
