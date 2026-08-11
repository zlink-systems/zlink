// SPDX-License-Identifier: MPL-2.0

import { Message, canShareNativeMessage } from '../../contracts/messaging/message';

export interface MessageSnapshot {
  data?: Buffer;
  nativeMessage?: unknown;
  refCount?: number;
  properties?: Readonly<Record<string, string>>;
  metadata?: Readonly<Map<number, Buffer>>;
}

export function messageFromSnapshot(snapshot: MessageSnapshot): Message {
  const message = Object.create(Message.prototype) as Message;
  const state = message as unknown as {
    _buffer: Buffer | undefined;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
    _nativeMessage?: unknown;
  };
  state._buffer = snapshot.data;
  state._refCount = snapshot.refCount ?? 1;
  state._properties = normalizeMessageProperties(snapshot.properties);
  state._metadata = snapshot.metadata ?? EMPTY_METADATA;
  state._nativeMessage = snapshot.nativeMessage;
  return message;
}

export function messageFromOwnedBuffer(data: Buffer, nativeMessage?: unknown): Message {
  const message = Object.create(Message.prototype) as Message;
  const state = message as unknown as {
    _buffer: Buffer;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
    _nativeMessage?: unknown;
  };
  state._buffer = data;
  state._refCount = 1;
  state._properties = EMPTY_PROPERTIES;
  state._metadata = EMPTY_METADATA;
  state._nativeMessage = nativeMessage;
  return message;
}

export function messageFromNativeFrame(
  nativeMessage: unknown,
  properties?: Readonly<Record<string, string>>
): Message {
  const message = Object.create(Message.prototype) as Message;
  const state = message as unknown as {
    _buffer: Buffer | undefined;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
    _nativeMessage?: unknown;
  };
  state._buffer = undefined;
  state._refCount = 1;
  state._properties = normalizeMessageProperties(properties);
  state._metadata = EMPTY_METADATA;
  state._nativeMessage = nativeMessage;
  return message;
}

export function messageFromOwnedRoutedBuffer(
  data: Buffer,
  routingId: Buffer,
  nativeMessage?: unknown
): Message {
  const identity = routingId.toString();
  return messageFromSnapshot({
    data,
    properties: {
      'Routing-Id': identity,
      Identity: identity
    },
    nativeMessage
  });
}

export function messageToSnapshot(message: Message): MessageSnapshot {
  const state = message as unknown as {
    _buffer: Buffer | undefined;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
    _nativeMessage?: unknown;
  };
  return {
    ...(state._buffer === undefined ? {} : { data: state._buffer }),
    refCount: state._refCount,
    properties: state._properties,
    metadata: state._metadata,
    ...(canShareNativeMessage(message) && state._nativeMessage !== undefined
      ? { nativeMessage: state._nativeMessage }
      : {})
  };
}

const EMPTY_PROPERTIES: Readonly<Record<string, string>> = Object.freeze({});
const EMPTY_METADATA: Readonly<Map<number, Buffer>> = Object.freeze(new Map<number, Buffer>());

function normalizeMessageProperties(
  properties?: Readonly<Record<string, string>>
): Readonly<Record<string, string>> {
  if (!properties) {
    return EMPTY_PROPERTIES;
  }
  return Object.isFrozen(properties) ? properties : Object.freeze(properties);
}
