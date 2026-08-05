// SPDX-License-Identifier: MPL-2.0

import { SendFlags } from '../../contracts/sockets/socket_constants';
import {
  Message,
  Received,
  RoutingId,
  TopicMessage
} from '../../contracts';
import { routingIdFromOwnedBuffer } from '../core/routing_id';
import {
  messageFromOwnedBuffer,
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

export interface NativeReceivedRaw {
  parts: MessageSnapshot[];
  routingId?: Buffer | null;
  requestSeq?: bigint | null;
}

export interface NativeTopicMessageRaw {
  topic: string;
  parts?: MessageSnapshot[];
  data?: Buffer;
  routingId?: Buffer | null;
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

function materializeTopicParts(raw: NativeTopicMessageRaw): Message[] {
  if (raw.data) {
    // Hot path: core SUB messages are normally single-part. The native layer
    // passes the owned payload Buffer directly so public TopicMessage adoption
    // avoids a per-message native parts array and snapshot object.
    return [messageFromOwnedBuffer(raw.data)];
  }
  return materializeParts(raw.parts ?? []);
}

function hasReplyableRequestSeq(requestSeq: bigint | null): requestSeq is bigint {
  return requestSeq !== null && requestSeq !== 0n;
}

export function materializeReceived(
  raw: NativeReceivedRaw,
  reply?: (requestSeq: bigint, parts: readonly Message[], flags: SendFlags) => void,
  send?: (parts: readonly Message[], flags: SendFlags) => boolean
): Received {
  const requestSeq = raw.requestSeq ?? null;
  return createReceived(
    materializeParts(raw.parts),
    wrapNativeRoutingId(raw.routingId ?? null),
    requestSeq,
    hasReplyableRequestSeq(requestSeq) && reply
      ? {
          beginReply() {
            return createReceivedReplyOperation(
              (parts: readonly Message[], flags: SendFlags): void => {
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
  reply?: (requestSeq: bigint, parts: readonly Message[], flags: SendFlags) => void,
  send?: (parts: readonly Message[], flags: SendFlags) => boolean
): void {
  const requestSeq = raw.requestSeq ?? null;
  replaceReceived(
    target,
    materializeParts(raw.parts),
    wrapNativeRoutingId(raw.routingId ?? null),
    requestSeq,
    hasReplyableRequestSeq(requestSeq) && reply
      ? {
          beginReply() {
            return createReceivedReplyOperation(
              (parts: readonly Message[], flags: SendFlags): void => {
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
