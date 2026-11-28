import { ZlinkStreamContentType } from '@zlink-systems/stream-wire';
import type { ZLinkBackendActorRef } from '../backend/contracts';
import { decodeActorMessageFollowContext } from '../actors';
import type { ZLinkActor } from '../../contracts';
import type { ZLinkActorHandoffPacket } from '../actors/actor-handoff';
import type { Message } from '../../contracts/Common/Message';
import { ZLinkBufferMessage as RuntimeMessage } from '../backend/runtime-message';
import { decodeChannelEnvelope } from '../channels/channel-envelope';
import { decodeRoutingId as decodeWireRoutingId, routingIdsEqual } from '../routing-id';
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

export interface ZLinkDecodedRemoteActorJoinRequest {
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
  readonly raw: boolean;
  readonly actorId: string;
  readonly actorType: string;
  readonly actorRef?: ZLinkBackendActorRef;
  readonly expectedMembershipEpoch: bigint;
  readonly actorCreateRequest?: Message;
  readonly request: Message;
  readonly requestContentType: string;
  readonly phase?: 'admission';
  readonly transferId?: string;
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
  return hasRemoteActorJoinIdentity(value) && typeof value.request === 'string';
}

export function decodeRemoteActorJoinPayload(
  payload: ZLinkRemoteActorJoinWirePayload & {
    readonly actorId: string;
    readonly actorType: string;
  },
  request: Message,
  raw: boolean,
  envelope?: ReturnType<typeof decodeChannelEnvelope>
): ZLinkDecodedRemoteActorJoinRequest {
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
    expectedMembershipEpoch:
      typeof payload.expectedMembershipEpoch === 'string'
        ? BigInt(payload.expectedMembershipEpoch)
        : 0n,
    actorCreateRequest:
      typeof payload.actorCreateRequest === 'string'
        ? RuntimeMessage.from(Buffer.from(payload.actorCreateRequest, 'base64'))
        : undefined,
    phase: payload.phase === 'admission' ? payload.phase : undefined,
    transferId: typeof payload.transferId === 'string' ? payload.transferId : undefined,
    request,
    requestContentType:
      typeof payload.requestContentType === 'string'
        ? payload.requestContentType
        : ZlinkStreamContentType.Json
  };
}

export function decodeHandoffBacklog(value: unknown): readonly ZLinkActorHandoffPacket[] {
  if (!Array.isArray(value)) return [];
  return value.map((entry, expectedIndex) => {
    if (
      typeof entry !== 'object' ||
      entry === null ||
      (entry as { index?: unknown }).index !== expectedIndex ||
      typeof (entry as { header?: unknown }).header !== 'string' ||
      typeof (entry as { payload?: unknown }).payload !== 'string' ||
      typeof (entry as { returnResponse?: unknown }).returnResponse !== 'boolean'
    ) {
      throw new Error('Remote actor handoff backlog is not a contiguous packet sequence.');
    }
    const packet = entry as ZLinkActorHandoffPacket;
    const messageFollowContext = decodeActorMessageFollowContext(packet.messageFollowContext);
    const source = packet.source;
    if (
      messageFollowContext === undefined ||
      (packet.returnResponse &&
        (source === undefined ||
          source.ownerId.length === 0 ||
          BigInt(source.ownerLeaseGeneration) <= 0n ||
          source.nodeRid.length === 0 ||
          BigInt(source.nodeGeneration) <= 0n ||
          source.replyRouteId !==
            (packet.messageFollowOrigin !== undefined &&
            routingIdsEqual(
              decodeWireRoutingId(source.nodeRid, source.nodeRidHex),
              packet.messageFollowOrigin.sourceNodeRid
            )
              ? packet.messageFollowOrigin.originalReplyRouteId.toString()
              : messageFollowContext.replyRouteId))) ||
      (!packet.returnResponse && source !== undefined)
    ) {
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
