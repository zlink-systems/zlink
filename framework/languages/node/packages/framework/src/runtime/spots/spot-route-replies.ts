import type {
  ZLinkBackendMessageLike as MessageLike,
  ZLinkBackendReceived as BackendReceived
} from '../backend/runtime-values';
import {
  decodeChannelEnvelope,
  encodeChannelErrorReplyParts,
  encodeChannelReplyParts
} from '../channels/channel-envelope';
import { ZLinkConfigurationException } from '../configuration';
import { encodeSpotRouteBridgeReply } from './spot-route-reply-wire';
import type { ZLinkDecodedRemoteActorJoinRequest } from './spot-remote-codec';

export interface ZLinkRouteReplySubmitOperation {
  message(message: MessageLike): ZLinkRouteReplySubmitOperation;
  submit(): void;
}

export function submitRouteReply(operation: ZLinkRouteReplySubmitOperation): boolean {
  try {
    operation.submit();
    return true;
  } catch (error) {
    if (isNativeBadAddress(error)) {
      return false;
    }
    throw error;
  }
}

export function submitRoutePayloadReply(
  received: BackendReceived,
  envelope: ReturnType<typeof decodeChannelEnvelope> | undefined,
  payload: unknown
): boolean {
  if (envelope === undefined) {
    return submitRouteReply(
      received.reply()
        .message(Buffer.from(JSON.stringify(payload)))
    );
  }
  return submitRouteReply(appendRouteReplyParts(
    received.reply(),
    encodeChannelReplyParts(envelope.header, payload)
  ));
}

export function submitSpotRouteBridgeReply(
  received: BackendReceived,
  envelope: ReturnType<typeof decodeChannelEnvelope> | undefined,
  payload: Parameters<typeof encodeSpotRouteBridgeReply>[0]
): boolean {
  return submitRoutePayloadReply(received, envelope, encodeSpotRouteBridgeReply(payload));
}

export function submitRoutedActorJoinReply(
  received: BackendReceived,
  decoded: ZLinkDecodedRemoteActorJoinRequest,
  payload: unknown
): boolean {
  if (decoded.raw) {
    return submitRouteReply(
      received.reply()
        .message(Buffer.from(JSON.stringify(payload)))
    );
  }
  return submitRouteReply(appendRouteReplyParts(
    received.reply(),
    encodeChannelReplyParts(requireRoutedActorJoinEnvelope(decoded).header, payload)
  ));
}

export function submitRoutedActorJoinError(
  received: BackendReceived,
  decoded: ZLinkDecodedRemoteActorJoinRequest,
  error: unknown
): boolean {
  if (decoded.raw) {
    return submitRoutedActorJoinReply(received, decoded, {
      accepted: false,
      actorNodeRid: '',
      actorId: decoded.actorId,
      actorGeneration: '0'
    });
  }
  return submitRouteReply(appendRouteReplyParts(
    received.reply(),
    encodeChannelErrorReplyParts(
      requireRoutedActorJoinEnvelope(decoded).header,
      error instanceof Error ? error.message : String(error)
    )
  ));
}

export function appendRouteReplyParts(
  operation: { message(message: MessageLike): ZLinkRouteReplySubmitOperation },
  parts: readonly MessageLike[]
): ZLinkRouteReplySubmitOperation {
  if (parts.length === 0) {
    throw new ZLinkConfigurationException('Spot route reply must contain at least one part.');
  }
  let current = operation.message(parts[0]);
  for (let index = 1; index < parts.length; index++) {
    current = current.message(parts[index]);
  }
  return current;
}

export function isReplyableRequestSeq(requestSeq: bigint | null): requestSeq is bigint {
  return requestSeq !== null && requestSeq !== 0n;
}

function requireRoutedActorJoinEnvelope(
  decoded: ZLinkDecodedRemoteActorJoinRequest
): ReturnType<typeof decodeChannelEnvelope> {
  if (decoded.envelope === undefined) {
    throw new ZLinkConfigurationException('Routed actor join channel envelope is missing.');
  }
  return decoded.envelope;
}

function isNativeBadAddress(error: unknown): boolean {
  return typeof error === 'object' &&
    error !== null &&
    Number((error as { nativeErrno?: unknown }).nativeErrno) === 14;
}
