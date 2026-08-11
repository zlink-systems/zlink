// SPDX-License-Identifier: MPL-2.0

import { SendFlags } from '../../contracts/sockets/socket_constants';
import {
  Message,
  Received,
  RoutingId,
  TopicMessage,
  type MessageLike
} from '../../contracts';
import { routingIdFromOwnedBuffer } from '../core/routing_id';
import {
  messageFromOwnedBuffer,
  messageFromOwnedRoutedBuffer,
  messageFromNativeFrame,
  messageFromSnapshot,
  type MessageSnapshot
} from './message_snapshot';
import {
  createReceived,
  replaceReceived
} from './received_state';
import {
  createReceivedReplyOperation,
  createReceivedSendOperation,
} from './received_operations';
import {
  createTopicMessage,
  replaceTopicMessage
} from './topic_message_state';

export interface NativeReceivedEnvelope {
  parts?: MessageSnapshot[];
  data?: Buffer;
  nativeMessage?: unknown;
  routingId?: Buffer | null;
  requestSeq?: bigint | null;
  transportPairId?: bigint;
  transportPairGeneration?: bigint;
}

/** Internal receive transfer: either an envelope or an unrouted native frame. */
export type NativeReceivedRaw = NativeReceivedEnvelope | object;

export interface NativeTopicMessageRaw {
  topic: string;
  parts?: MessageSnapshot[];
  data?: Buffer;
  nativeMessage?: unknown;
  routingId?: Buffer | null;
}

export interface RoutedReceiveOperations {
  send(routingId: Buffer, parts: readonly Message[], flags: SendFlags): boolean;
  reply(routingId: Buffer, requestSeq: bigint,
        parts: readonly MessageLike[], flags: SendFlags): void;
}

interface RoutedReceiveContext {
  routingId: Buffer | null;
  requestSeq: bigint | null;
  cachedRoutingBytes: Buffer | null;
  cachedRoutingId: RoutingId | null;
  operations: RoutedReceiveOperations;
  sendContext: { beginSend(): ReturnType<typeof createReceivedSendOperation> };
  replyContext: { beginReply(): ReturnType<typeof createReceivedReplyOperation> };
}

const routedReceiveContexts = new WeakMap<Received, RoutedReceiveContext>();

function envelopeOf(raw: NativeReceivedRaw): NativeReceivedEnvelope | null {
  const candidate = raw as NativeReceivedEnvelope;
  return candidate.data !== undefined
      || candidate.nativeMessage !== undefined
      || candidate.parts !== undefined
      || candidate.routingId !== undefined
      || candidate.requestSeq !== undefined
    ? candidate
    : null;
}

export function nativeReceivedRoutingId(raw: NativeReceivedRaw): Buffer | null {
  return envelopeOf(raw)?.routingId ?? null;
}

function wrapNativeRoutingId(routingId: Buffer | null | undefined): RoutingId | null {
  if (!routingId || routingId.length === 0) {
    return null;
  }
  return routingIdFromOwnedBuffer(routingId);
}

function materializeParts(parts: MessageSnapshot[]): Message[] {
  if (parts.length === 1) {
    return [messageFromSnapshot(parts[0])];
  }
  const messages = new Array<Message>(parts.length);
  for (let i = 0; i < parts.length; i += 1) {
    messages[i] = messageFromSnapshot(parts[i]);
  }
  return messages;
}

function materializeReceivedParts(raw: NativeReceivedRaw): Message[] {
  const envelope = envelopeOf(raw);
  if (envelope === null) {
    return [messageFromNativeFrame(raw)];
  }
  if (envelope.data !== undefined) {
    return envelope.routingId && envelope.routingId.length > 0
      ? [messageFromOwnedRoutedBuffer(envelope.data, envelope.routingId, envelope.nativeMessage)]
      : [messageFromOwnedBuffer(envelope.data, envelope.nativeMessage)];
  }
  if (envelope.nativeMessage !== undefined) {
    const identity = envelope.routingId && envelope.routingId.length > 0
      ? envelope.routingId.toString()
      : undefined;
    return [messageFromNativeFrame(
      envelope.nativeMessage,
      identity === undefined ? undefined : { 'Routing-Id': identity, Identity: identity }
    )];
  }
  return materializeParts(envelope.parts ?? []);
}

function materializeTopicParts(raw: NativeTopicMessageRaw): Message[] {
  if (raw.data) {
    // Hot path: core SUB messages are normally single-part. The native layer
    // passes the owned payload Buffer directly so public TopicMessage adoption
    // avoids a per-message native parts array and snapshot object.
    return [messageFromOwnedBuffer(raw.data)];
  }
  if (raw.nativeMessage !== undefined) {
    return [messageFromNativeFrame(raw.nativeMessage)];
  }
  return materializeParts(raw.parts ?? []);
}

function hasReplyableRequestSeq(requestSeq: bigint | null): requestSeq is bigint {
  return requestSeq !== null && requestSeq !== 0n;
}

export function materializeReceived(
  raw: NativeReceivedRaw,
  reply?: (requestSeq: bigint, parts: readonly MessageLike[], flags: SendFlags) => void,
  send?: (parts: readonly Message[], flags: SendFlags) => boolean
): Received {
  const envelope = envelopeOf(raw);
  const requestSeq = envelope?.requestSeq ?? null;
  return createReceived(
    materializeReceivedParts(raw),
    wrapNativeRoutingId(envelope?.routingId ?? null),
    requestSeq,
    hasReplyableRequestSeq(requestSeq) && reply
      ? {
          beginReply() {
            return createReceivedReplyOperation(
              (parts: readonly MessageLike[], flags: SendFlags): void => {
                reply(requestSeq, parts, flags);
              }
            );
          }
        }
      : null,
    send
      ? {
          beginSend() {
            return createReceivedSendOperation(
              (parts: readonly Message[], flags: SendFlags): boolean =>
                send(parts, flags)
            );
          }
        }
      : null
  );
}

export function materializeReceivedInto(
  target: Received,
  raw: NativeReceivedRaw,
  reply?: (requestSeq: bigint, parts: readonly MessageLike[], flags: SendFlags) => void,
  send?: (parts: readonly Message[], flags: SendFlags) => boolean
): void {
  const envelope = envelopeOf(raw);
  const requestSeq = envelope?.requestSeq ?? null;
  replaceReceived(
    target,
    materializeReceivedParts(raw),
    wrapNativeRoutingId(envelope?.routingId ?? null),
    requestSeq,
    hasReplyableRequestSeq(requestSeq) && reply
      ? {
          beginReply() {
            return createReceivedReplyOperation(
              (parts: readonly MessageLike[], flags: SendFlags): void => {
                reply(requestSeq, parts, flags);
              }
            );
          }
        }
      : null,
    send
      ? {
          beginSend() {
            return createReceivedSendOperation(
              (parts: readonly Message[], flags: SendFlags): boolean =>
                send(parts, flags)
            );
          }
        }
      : null
  );
  const targetInternal = target as Received & {
    transportPairId?: bigint;
    transportPairGeneration?: bigint;
  };
  targetInternal.transportPairId = envelope?.transportPairId;
  targetInternal.transportPairGeneration = envelope?.transportPairGeneration;
}

export function materializeRoutedReceivedInto(
  target: Received,
  raw: NativeReceivedRaw,
  operations: RoutedReceiveOperations
): void {
  const envelope = envelopeOf(raw);
  if (envelope === null) {
    throw new Error('routed receive requires a receive envelope');
  }
  let context = routedReceiveContexts.get(target);
  if (!context) {
    context = {
      routingId: null,
      requestSeq: null,
      cachedRoutingBytes: null,
      cachedRoutingId: null,
      operations,
      sendContext: {
        beginSend() {
          const routingId = context!.routingId;
          const operations = context!.operations;
          if (routingId == null) throw new Error('missing routed send target');
          return createReceivedSendOperation((parts, flags) =>
            operations.send(routingId, parts, flags));
        }
      },
      replyContext: {
        beginReply() {
          const routingId = context!.routingId;
          const requestSeq = context!.requestSeq;
          const operations = context!.operations;
          if (routingId == null || requestSeq == null || requestSeq === 0n) {
            throw new Error('missing request reply target');
          }
          return createReceivedReplyOperation((parts, flags) =>
            operations.reply(routingId, requestSeq, parts, flags));
        }
      }
    };
    routedReceiveContexts.set(target, context);
  }
  context.routingId = envelope.routingId ?? null;
  context.requestSeq = envelope.requestSeq ?? null;
  context.operations = operations;
  if (context.routingId === null) {
    context.cachedRoutingBytes = null;
    context.cachedRoutingId = null;
  } else if (context.cachedRoutingBytes === null
      || !context.cachedRoutingBytes.equals(context.routingId)) {
    context.cachedRoutingBytes = context.routingId;
    context.cachedRoutingId = wrapNativeRoutingId(context.routingId);
  }
  replaceReceived(
    target,
    materializeReceivedParts(raw),
    context.cachedRoutingId,
    context.requestSeq,
    hasReplyableRequestSeq(context.requestSeq) ? context.replyContext : null,
    context.routingId == null ? null : context.sendContext
  );
  const targetInternal = target as Received & {
    transportPairId?: bigint;
    transportPairGeneration?: bigint;
  };
  targetInternal.transportPairId = envelope.transportPairId;
  targetInternal.transportPairGeneration = envelope.transportPairGeneration;
}

export function materializeTopicMessage(raw: NativeTopicMessageRaw): TopicMessage {
  return createTopicMessage(
    raw.topic,
    materializeTopicParts(raw),
    wrapNativeRoutingId(raw.routingId ?? null)
  );
}

export function adoptTopicMessage(result: TopicMessage, raw: NativeTopicMessageRaw): void {
  replaceTopicMessage(
    result,
    raw.topic,
    materializeTopicParts(raw),
    wrapNativeRoutingId(raw.routingId ?? null)
  );
}
