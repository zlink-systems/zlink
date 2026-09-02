// SPDX-License-Identifier: MPL-2.0

import {
  Message,
  Received,
  ReplyToken,
  RoutingId,
  TopicMessage,
  type MessageLike
} from '../../contracts';
import { hasObservedManagedReceiveData } from '../../contracts/messaging/message';
import { normalizeRoutingId, routingIdFromOwnedBuffer } from '../core/routing_id';
import {
  createReplyToken,
} from '../../contracts/messaging/received';
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
  replaceTopicMessage,
  replaceTopicMessageOwnedSinglePart
} from './topic_message_state';

export interface NativeReceivedEnvelope {
  parts?: MessageSnapshot[];
  data?: Buffer;
  nativeMessage?: unknown;
  routingId?: Buffer | null;
  replyToken?: bigint | null;
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

type NativeTopicMessageSinglePart = readonly [Buffer, string];
type NativeTopicMessageEnvelope = NativeTopicMessageRaw | NativeTopicMessageSinglePart;

function isNativeTopicMessageSinglePart(
  raw: NativeTopicMessageEnvelope
): raw is NativeTopicMessageSinglePart {
  return Array.isArray(raw);
}

export interface RoutedReceiveOperations {
  readonly replyOwner: object;
  send(routingId: Buffer, parts: readonly Message[]): void;
  sendManaged(routingId: Buffer, parts: readonly Message[]): Promise<void>;
  reply(routingId: Buffer, token: ReplyToken, parts: readonly MessageLike[]): void;
}

interface RoutedReceiveContext {
  routingId: Buffer | null;
  replyToken: ReplyToken | null;
  operations: RoutedReceiveOperations;
  sendContext: { beginSend(): ReturnType<typeof createReceivedSendOperation> };
  replyContext: { beginReply(): ReturnType<typeof createReceivedReplyOperation> };
}

const routedReceiveContexts = new WeakMap<Received, RoutedReceiveContext>();

export function routedReceivedPrefersManagedBuffer(target: Received): boolean {
  const parts = target.parts;
  // HOT PATH: a reusable Received reveals its stable consumer role. A data
  // reader should avoid a second addon call on the next receive, while a
  // relay consumes the native frame and therefore stays on the movable path.
  return parts.length === 1
    && parts[0].size() <= 64
    && hasObservedManagedReceiveData(parts[0]);
}

export function routedReceivedRoutingBytes(target: Received): Buffer | null {
  return target.routingId === null ? null : normalizeRoutingId(target.routingId);
}

function envelopeOf(raw: NativeReceivedRaw): NativeReceivedEnvelope | null {
  const candidate = raw as NativeReceivedEnvelope;
  return candidate.data !== undefined
      || candidate.nativeMessage !== undefined
      || candidate.parts !== undefined
      || candidate.routingId !== undefined
      || candidate.replyToken !== undefined
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
    if (Buffer.isBuffer(raw)) {
      return [messageFromOwnedBuffer(raw)];
    }
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
      identity === undefined ? undefined : { 'Routing-Id': identity, Identity: identity },
      true
    )];
  }
  return materializeParts(envelope.parts ?? []);
}

function materializeTopicParts(raw: NativeTopicMessageEnvelope): Message[] {
  if (isNativeTopicMessageSinglePart(raw)) {
    return [messageFromOwnedBuffer(raw[0])];
  }
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

export function materializeReceived(
  raw: NativeReceivedRaw,
  send?: (parts: readonly Message[]) => void,
  sendManaged?: (parts: readonly Message[]) => Promise<void>
): Received {
  const envelope = envelopeOf(raw);
  return createReceived(
    materializeReceivedParts(raw),
    wrapNativeRoutingId(envelope?.routingId ?? null),
    null,
    null,
    send && sendManaged
      ? {
          beginSend() {
            return createReceivedSendOperation(
              (parts: readonly Message[]): void => send(parts),
              (parts: readonly Message[]): Promise<void> => sendManaged(parts)
            );
          }
        }
      : null
  );
}

export function materializeReceivedInto(
  target: Received,
  raw: NativeReceivedRaw,
  send?: (parts: readonly Message[]) => void,
  sendManaged?: (parts: readonly Message[]) => Promise<void>
): void {
  const envelope = envelopeOf(raw);
  replaceReceived(
      target,
      materializeReceivedParts(raw),
      wrapNativeRoutingId(envelope?.routingId ?? null),
      null,
      null,
      send && sendManaged
        ? {
            beginSend() {
              return createReceivedSendOperation(
                (parts: readonly Message[]): void => send(parts),
                (parts: readonly Message[]): Promise<void> => sendManaged(parts)
              );
            }
          }
        : null
    );
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
      replyToken: null,
      operations,
      sendContext: {
        beginSend() {
          const routingId = context!.routingId;
          const operations = context!.operations;
          if (routingId == null) throw new Error('missing routed send target');
          return createReceivedSendOperation(
            (parts) => operations.send(routingId, parts),
            (parts) => operations.sendManaged(routingId, parts)
          );
        }
      },
      replyContext: {
        beginReply() {
          const routingId = context!.routingId;
          const replyToken = context!.replyToken;
          const operations = context!.operations;
          if (routingId == null || replyToken == null) {
            throw new Error('missing request reply target');
          }
          return createReceivedReplyOperation((parts) =>
            operations.reply(routingId, replyToken, parts));
        }
      }
    };
    routedReceiveContexts.set(target, context);
  }
  context.routingId = envelope.routingId ?? null;
  context.operations = operations;
  const rawReplyToken = envelope.replyToken ?? 0n;
  context.replyToken = rawReplyToken === 0n
    ? null
    : createReplyToken(operations.replyOwner, rawReplyToken);
  const routingId = context.routingId === null
    ? null
    : target.routingId !== null
      && normalizeRoutingId(target.routingId).equals(context.routingId)
      ? target.routingId
      : wrapNativeRoutingId(context.routingId);
  replaceReceived(
      target,
      materializeReceivedParts(raw),
      routingId,
      context.replyToken,
      context.replyToken ? context.replyContext : null,
      context.routingId == null ? null : context.sendContext
    );
}

export function materializeTopicMessage(raw: NativeTopicMessageEnvelope): TopicMessage {
  const topic = isNativeTopicMessageSinglePart(raw) ? raw[1] : raw.topic;
  const routingId = isNativeTopicMessageSinglePart(raw)
    ? null
    : wrapNativeRoutingId(raw.routingId ?? null);
  return createTopicMessage(
    topic,
    materializeTopicParts(raw),
    routingId
  );
}

export function adoptTopicMessage(result: TopicMessage, raw: NativeTopicMessageEnvelope): void {
  const topic = isNativeTopicMessageSinglePart(raw) ? raw[1] : raw.topic;
  const routingId = isNativeTopicMessageSinglePart(raw)
    ? null
    : wrapNativeRoutingId(raw.routingId ?? null);
  const data = isNativeTopicMessageSinglePart(raw)
      ? raw[0]
      : raw.data;
    if (data !== undefined) {
      replaceTopicMessageOwnedSinglePart(result, topic, data, routingId);
    } else {
      replaceTopicMessage(
        result,
        topic,
        materializeTopicParts(raw),
        routingId
      );
    }
}
