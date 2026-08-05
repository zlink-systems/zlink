import type { Message } from '../../contracts/Common/Message';
import type { ActorRef } from '../../contracts';
import {
  decodeChannelEnvelope,
  decodeChannelPayload,
  type ZLinkChannelEnvelopeCodecRegistry
} from '../channels/channel-envelope';
import {
  attachActorMessageFollowContext,
  ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
  ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET
} from '../actors';
import { decodeWireRoutingId } from '../actors/actor-remote-wire';
import {
  decodeRemoteBoundSessionErrorPayload,
  decodeRemoteBoundSessionSealPayload,
  decodeRemoteBoundSessionOwnershipPayload,
  decodeRemoteBoundSessionResponsePayload,
  decodeRemoteBoundSessionSendPayload
} from '../actors/bound-session-wire';
import { decodeRemoteActorPacketRelayPayload } from '../actors/actor-packet-relay-wire';

export interface ZLinkRemoteBoundSessionSend {
  readonly actorId: string;
  readonly message: unknown;
  readonly packetName?: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly actorRef?: ActorRef;
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
  readonly flowId?: string;
  readonly flowOrigin?: import('../../contracts').ZLinkFlowOrigin;
  readonly actorPacketTarget?: unknown;
}

export interface ZLinkRemoteBoundSessionResponse {
  readonly actorId: string;
  readonly message: unknown;
  readonly packetName: string;
  readonly requestSeq: bigint;
  readonly metadata: ReadonlyMap<string, string>;
  readonly compressPayload: boolean;
  readonly actorPacketTarget?: unknown;
}

export interface ZLinkRemoteBoundSessionError {
  readonly actorId: string;
  readonly error: unknown;
  readonly packetName: string;
  readonly requestSeq: bigint;
  readonly metadata: ReadonlyMap<string, string>;
  readonly actorPacketTarget?: unknown;
}

export interface ZLinkRemoteBoundSessionOwnership {
  readonly actorId: string;
  readonly actorNodeRid: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration: string;
  readonly previousActorOwnershipGeneration: string;
  readonly actorOwnershipGeneration: string;
  readonly bindingGeneration: string;
  readonly previousOwnerLeaseGeneration: string;
  readonly targetOwnerLeaseGeneration: string;
  readonly acceptedHighWater: string;
  readonly sealId: string;
  readonly acceptedJournalReference: string;
  readonly acceptedJournalChecksumCrc32c: number;
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
}

export interface ZLinkRemoteBoundSessionSeal {
  readonly actorId: string;
  readonly actorGeneration: string;
  readonly actorOwnershipGeneration: string;
  readonly bindingGeneration: string;
  readonly ownerLeaseGeneration: string;
  readonly sealId: string;
  readonly abort: boolean;
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
}

export function decodeRemoteBoundSessionSeal(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteBoundSessionSeal | undefined {
  try {
    const decoded = decodeMultipartPayload(parts, codecs);
    if (
      decoded.packetName !== ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET &&
      decoded.packetName !== ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET
    ) return undefined;
    return {
      ...decodeRemoteBoundSessionSealPayload(decoded.payload),
      envelope: decoded.envelope
    };
  } catch {
    return undefined;
  }
}

export function decodeRemoteBoundSessionOwnership(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteBoundSessionOwnership | undefined {
  try {
    const decoded = decodeMultipartPayload(parts, codecs);
    if (decoded.packetName !== ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET) return undefined;
    return {
      ...decodeRemoteBoundSessionOwnershipPayload(decoded.payload),
      envelope: decoded.envelope
    };
  } catch {
    return undefined;
  }
}

export interface ZLinkRemoteActorPacketRelay {
  readonly actorId: string;
  readonly routerChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionSpotId?: string;
  readonly header: string;
  readonly payload: string;
  readonly actorRef?: ActorRef;
  readonly bindingActorRef?: ActorRef;
  readonly messageFollowContext?: import('../actors').ZLinkActorMessageFollowContext;
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
}

export function decodeRemoteBoundSessionSend(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteBoundSessionSend | undefined {
  try {
    const decoded = decodeMultipartPayload(parts, codecs);
    if (decoded.packetName !== ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET) return undefined;
    const send = decodeRemoteBoundSessionSendPayload(decoded.payload);
    return {
      actorId: send.actorId,
      message: send.message,
      packetName: send.boundPacketName,
      metadata: metadataOf(send.metadata),
      actorRef: decodeActorRef({
        actorId: send.actorId,
        actorMeshName: send.actorMeshName,
        actorNodeRid: send.actorNodeRid,
        actorNodeRidHex: send.actorNodeRidHex,
        actorGeneration: send.actorGeneration,
        actorOwnershipGeneration: send.actorOwnershipGeneration
      }),
      envelope: decoded.envelope,
      flowId: send.flowId,
      flowOrigin: send.flowOrigin,
      actorPacketTarget: send.actorPacketTarget
    };
  } catch {
    return undefined;
  }
}

function decodeActorRef(payload: {
  readonly actorId?: unknown;
  readonly actorMeshName?: unknown;
  readonly actorNodeRid?: unknown;
  readonly actorNodeRidHex?: unknown;
  readonly actorGeneration?: unknown;
  readonly actorOwnershipGeneration?: unknown;
}): ActorRef | undefined {
  if (
    typeof payload.actorId !== 'string'
    || typeof payload.actorNodeRid !== 'string'
    || typeof payload.actorGeneration !== 'string'
    || typeof payload.actorMeshName !== 'string'
  ) {
    return undefined;
  }
  const actorRef = {
    actorId: payload.actorId,
    objectGeneration: BigInt(payload.actorGeneration),
    meshName: payload.actorMeshName,
    nodeRid: decodeWireRoutingId(
      payload.actorNodeRid,
      typeof payload.actorNodeRidHex === 'string' ? payload.actorNodeRidHex : undefined
    )
  } as ActorRef & { ownershipGeneration?: bigint };
  if (typeof payload.actorOwnershipGeneration === 'string') {
    actorRef.ownershipGeneration = BigInt(payload.actorOwnershipGeneration);
  }
  return actorRef;
}

export function decodeRemoteBoundSessionResponse(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteBoundSessionResponse | undefined {
  const decoded = decodeRemoteBoundSessionControl(parts, ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET, codecs);
  if (decoded === undefined) {
    return undefined;
  }
  return {
    actorId: decoded.actorId,
    message: decoded.payload.message,
    packetName: decoded.packetName,
    requestSeq: decoded.requestSeq,
    metadata: decoded.metadata,
    compressPayload: decoded.payload.compressPayload === true,
    actorPacketTarget: decoded.actorPacketTarget
  };
}

export function decodeRemoteBoundSessionError(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteBoundSessionError | undefined {
  const decoded = decodeRemoteBoundSessionControl(parts, ZLINK_REMOTE_BOUND_SESSION_ERROR_PACKET, codecs);
  if (decoded === undefined) {
    return undefined;
  }
  return {
    actorId: decoded.actorId,
    error: decoded.payload.error,
    packetName: decoded.packetName,
    requestSeq: decoded.requestSeq,
    metadata: decoded.metadata,
    actorPacketTarget: decoded.actorPacketTarget
  };
}

export function decodeRemoteActorPacketRelay(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): ZLinkRemoteActorPacketRelay | undefined {
  try {
    const multipart = decodeMultipartPayload(parts, codecs);
    if (multipart.packetName !== ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET) return undefined;
    const relay = decodeRemoteActorPacketRelayPayload(multipart.payload);
    return {
      ...relay,
      actorRef: decodeForwardedActorRef({
        actorId: relay.actorId,
        actorNodeRid: relay.actorNodeRid,
        actorNodeRidHex: relay.actorNodeRidHex,
        actorGeneration: relay.actorGeneration
      }, relay.messageFollowContext),
      bindingActorRef: decodeActorRef({
        actorId: relay.actorId,
        actorNodeRid: relay.bindingActorNodeRid,
        actorNodeRidHex: relay.bindingActorNodeRidHex,
        actorGeneration: relay.bindingActorGeneration
      }),
      messageFollowContext: relay.messageFollowContext,
      envelope: multipart.envelope
    };
  } catch {
    return undefined;
  }
}

function decodeForwardedActorRef(payload: {
  readonly actorId?: unknown;
  readonly actorNodeRid?: unknown;
  readonly actorNodeRidHex?: unknown;
  readonly actorGeneration?: unknown;
}, messageFollowContext?: import('../actors').ZLinkActorMessageFollowContext): ActorRef | undefined {
  const actorRef = decodeActorRef(payload);
  return actorRef === undefined || messageFollowContext === undefined
    ? actorRef
    : attachActorMessageFollowContext(actorRef, messageFollowContext);
}

function decodeRemoteBoundSessionControl(
  parts: readonly Message[],
  packetName: string,
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): {
  readonly actorId: string;
  readonly packetName: string;
  readonly requestSeq: bigint;
  readonly metadata: ReadonlyMap<string, string>;
  readonly payload: {
    readonly message?: unknown;
    readonly error?: unknown;
    readonly compressPayload?: unknown;
  };
  readonly actorPacketTarget?: unknown;
} | undefined {
  try {
    const multipart = decodeMultipartPayload(parts, codecs);
    if (multipart.packetName !== packetName) return undefined;
    const payload = multipart.payload as Record<string, unknown>;
    const decoded = packetName === ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET
      ? decodeRemoteBoundSessionResponsePayload(payload)
      : decodeRemoteBoundSessionErrorPayload(payload);
    return {
      actorId: decoded.actorId,
      packetName: decoded.boundPacketName,
      requestSeq: decoded.requestSeq,
      metadata: metadataOf(decoded.metadata),
      payload,
      actorPacketTarget: decoded.actorPacketTarget
    };
  } catch {
    return undefined;
  }
}

function decodeMultipartPayload(
  parts: readonly Message[],
  codecs?: ZLinkChannelEnvelopeCodecRegistry
): {
  readonly packetName?: string;
  readonly payload: unknown;
  readonly envelope?: ReturnType<typeof decodeChannelEnvelope>;
} {
  if (parts.length === 1) {
    const payload = JSON.parse(parts[0].data().toString()) as { readonly packetName?: unknown };
    return {
      packetName: typeof payload.packetName === 'string' ? payload.packetName : undefined,
      payload
    };
  }
  const envelope = decodeChannelEnvelope(parts);
  const decodedPayload = decodeChannelPayload(envelope, codecs);
  return {
    packetName: envelope.packetName,
    payload: typeof decodedPayload === 'object' && decodedPayload !== null
      ? { ...decodedPayload, packetName: envelope.packetName }
      : decodedPayload,
    envelope
  };
}

function metadataOf(value: unknown): ReadonlyMap<string, string> {
  return new Map(Object.entries(
    typeof value === 'object' && value !== null
      ? value as Record<string, string>
      : {}
  ));
}
