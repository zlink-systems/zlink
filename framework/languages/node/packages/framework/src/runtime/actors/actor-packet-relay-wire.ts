import type { ActorRef, RoutingId, ZLinkSessionActor } from '../../contracts';
import { ZLinkSpotKind } from '../../contracts';
import { decodeRoutingId, routingIdWireHex } from '../routing-id';
import type { ZLinkRemoteActorPacketTarget } from './actor-runtime-state';
import {
  decodeActorMessageFollowContext,
  type ZLinkActorMessageFollowContext
} from './actor-message-follow-context';

export const ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET = '__zlink.actor.packet.relay';
export const ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET = 'zlink.framework.actor.session_disconnected';
export const ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET = 'framework.internal.actor-session-bind';

export function encodeRemoteActorSessionBinding(input: {
  readonly sessionNodeRid: RoutingId;
  readonly sessionRid: RoutingId;
}): Uint8Array {
  return Buffer.from(JSON.stringify({
    sessionNodeRid: String(input.sessionNodeRid),
    sessionNodeRidHex: routingIdWireHex(input.sessionNodeRid),
    sessionRid: String(input.sessionRid),
    sessionRidHex: routingIdWireHex(input.sessionRid)
  }));
}

export function decodeRemoteActorSessionBinding(payload: Uint8Array): {
  readonly sessionNodeRid: RoutingId;
  readonly sessionRid: RoutingId;
} {
  const decoded = JSON.parse(Buffer.from(payload).toString('utf8')) as Record<string, unknown>;
  if (typeof decoded.sessionNodeRid !== 'string' || typeof decoded.sessionRid !== 'string') {
    throw new Error('Remote actor session binding payload is invalid.');
  }
  return {
    sessionNodeRid: decodeRoutingId(decoded.sessionNodeRid, decoded.sessionNodeRidHex),
    sessionRid: decodeRoutingId(decoded.sessionRid, decoded.sessionRidHex)
  };
}

export function encodeRemoteActorPacketRelayPayload(input: {
  readonly actorId: string;
  readonly routerChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionSpotId?: string;
  readonly header: Uint8Array;
  readonly payload: Uint8Array;
  readonly bindingActorRef?: ActorRef;
  /** The actor reference used to preserve relocation context at the target Spot. */
  readonly actorRef?: ActorRef;
  readonly returnResponse?: boolean;
  readonly messageFollowContext?: ZLinkActorMessageFollowContext;
}): Record<string, unknown> {
  return {
    packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET,
    actorId: input.actorId,
    routerChannelId: input.routerChannelId,
    boundSessionTargetNodeRid: input.boundSessionTargetNodeRid,
    boundSessionSpotId: input.boundSessionSpotId,
    actorNodeRid: input.actorRef === undefined ? undefined : String(input.actorRef.nodeRid),
    actorNodeRidHex: input.actorRef === undefined
      ? undefined
      : routingIdWireHex(input.actorRef.nodeRid),
    actorGeneration: input.actorRef?.objectGeneration.toString(),
    bindingActorNodeRid: input.bindingActorRef === undefined ? undefined : String(input.bindingActorRef.nodeRid),
    bindingActorNodeRidHex: input.bindingActorRef === undefined
      ? undefined
      : routingIdWireHex(input.bindingActorRef.nodeRid),
    bindingActorGeneration: input.bindingActorRef?.objectGeneration.toString(),
    bindingGeneration: (input.bindingActorRef as (ActorRef & {
      readonly bindingGeneration?: bigint;
    }) | undefined)?.bindingGeneration?.toString(),
    returnResponse: input.returnResponse,
    messageFollowContext: input.messageFollowContext,
    header: Buffer.from(input.header).toString('base64'),
    payload: Buffer.from(input.payload).toString('base64')
  };
}

export function encodeMessageFollowRemoteActorPacketRelayPayload(input: {
  readonly actorId: string;
  readonly routerChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionSpotId?: string;
  readonly header: string;
  readonly payload: string;
  readonly actorNodeRid: string;
  readonly actorGeneration: string;
  readonly returnResponse: boolean;
  readonly messageFollowContext: ZLinkActorMessageFollowContext;
}): Record<string, unknown> {
  return { packetName: ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET, ...input };
}

export interface ZLinkRemoteActorPacketTargetWire {
  readonly routerChannelId: string;
  readonly targetNodeRid: string;
  readonly targetNodeRidHex?: string;
  readonly spotId: string;
  readonly spotKind: ZLinkSpotKind;
  readonly targetSpotGeneration?: string;
}

export function encodeRemoteActorPacketTarget(
  target: ZLinkRemoteActorPacketTarget | undefined
): ZLinkRemoteActorPacketTargetWire | undefined {
  if (target === undefined) {
    return undefined;
  }
  return {
    routerChannelId: target.routerChannelId,
    targetNodeRid: String(target.targetNodeRid),
    targetNodeRidHex: routingIdWireHex(target.targetNodeRid),
    spotId: String(target.spotId),
    spotKind: target.spotKind ?? ZLinkSpotKind.User,
    targetSpotGeneration: target.targetSpotGeneration?.toString()
  };
}

export function decodeRemoteActorPacketTarget(value: unknown): ZLinkRemoteActorPacketTarget | undefined {
  if (
    typeof value !== 'object' ||
    value === null ||
    typeof (value as { routerChannelId?: unknown }).routerChannelId !== 'string' ||
    typeof (value as { targetNodeRid?: unknown }).targetNodeRid !== 'string' ||
    typeof (value as { spotId?: unknown }).spotId !== 'string'
  ) {
    return undefined;
  }
  return {
    routerChannelId: (value as { routerChannelId: string }).routerChannelId,
    targetNodeRid: decodeRoutingId(
      (value as { targetNodeRid: string }).targetNodeRid,
      (value as { targetNodeRidHex?: unknown }).targetNodeRidHex
    ),
    spotId: requireSpotId((value as { spotId: string }).spotId),
    spotKind: (value as { spotKind?: unknown }).spotKind === ZLinkSpotKind.Entry
      ? ZLinkSpotKind.Entry
      : ZLinkSpotKind.User,
    targetSpotGeneration:
      typeof (value as { targetSpotGeneration?: unknown }).targetSpotGeneration === 'string'
        ? BigInt((value as { targetSpotGeneration: string }).targetSpotGeneration)
        : undefined
  };
}

function requireSpotId(value: string): string {
  const bytes = Buffer.byteLength(value, 'utf8');
  if (bytes < 1 || bytes > 255) {
    throw new Error('Actor packet target SpotId must contain 1..255 UTF-8 bytes.');
  }
  return value;
}

export function sessionActorPacketTargetKey(actor: ZLinkSessionActor): string {
  return `${String(actor.ref.nodeRid)}:${actor.actorId}:${String(actor.ref.objectGeneration)}`;
}

export function decodeRemoteActorPacketRelayPayload(payload: unknown): {
  readonly actorId: string;
  readonly routerChannelId?: string;
  readonly boundSessionTargetNodeRid?: string;
  readonly boundSessionSpotId?: string;
  readonly header: string;
  readonly payload: string;
  readonly actorNodeRid?: string;
  readonly actorNodeRidHex?: string;
  readonly actorGeneration?: string;
  readonly bindingActorNodeRid?: string;
  readonly bindingActorNodeRidHex?: string;
  readonly bindingActorGeneration?: string;
  readonly bindingGeneration?: string;
  readonly returnResponse?: boolean;
  readonly messageFollowContext?: ZLinkActorMessageFollowContext;
} {
  if (
    typeof payload !== 'object' ||
    payload === null ||
    (payload as { packetName?: unknown }).packetName !== ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET ||
    typeof (payload as { actorId?: unknown }).actorId !== 'string' ||
    typeof (payload as { header?: unknown }).header !== 'string' ||
    typeof (payload as { payload?: unknown }).payload !== 'string'
  ) {
    throw new Error('Remote actor packet relay payload is invalid.');
  }
  return {
    actorId: (payload as { actorId: string }).actorId,
    routerChannelId: optionalString(payload, 'routerChannelId'),
    boundSessionTargetNodeRid: optionalString(payload, 'boundSessionTargetNodeRid'),
    boundSessionSpotId: optionalString(payload, 'boundSessionSpotId'),
    header: (payload as { header: string }).header,
    payload: (payload as { payload: string }).payload,
    actorNodeRid: optionalString(payload, 'actorNodeRid'),
    actorNodeRidHex: optionalString(payload, 'actorNodeRidHex'),
    actorGeneration: optionalString(payload, 'actorGeneration'),
    bindingActorNodeRid: optionalString(payload, 'bindingActorNodeRid'),
    bindingActorNodeRidHex: optionalString(payload, 'bindingActorNodeRidHex'),
    bindingActorGeneration: optionalString(payload, 'bindingActorGeneration'),
    bindingGeneration: optionalString(payload, 'bindingGeneration'),
    returnResponse: (payload as { returnResponse?: unknown }).returnResponse === true,
    messageFollowContext: decodeActorMessageFollowContext(
      (payload as { messageFollowContext?: unknown }).messageFollowContext
    )
  };
}

function optionalString(value: object, key: string): string | undefined {
  const field = (value as Record<string, unknown>)[key];
  return typeof field === 'string' ? field : undefined;
}
