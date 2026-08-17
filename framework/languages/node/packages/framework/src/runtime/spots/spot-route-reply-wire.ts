import type { ZLinkRemoteActorPacketTargetWire } from '../actors/actor-packet-relay-wire';

export interface ZLinkSpotRouteBridgeReplyPayload {
  readonly ok: boolean;
  readonly response?: unknown;
  readonly error?: unknown;
  readonly errorKind?: unknown;
  readonly deferredResponse?: boolean;
  readonly actorPacketTarget?: ZLinkRemoteActorPacketTargetWire;
}

export function encodeSpotRouteBridgeReply(payload: ZLinkSpotRouteBridgeReplyPayload): ZLinkSpotRouteBridgeReplyPayload {
  return payload;
}

