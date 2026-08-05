// SPDX-License-Identifier: MPL-2.0

import { Message } from '../../contracts';

export interface MessageSnapshot {
  data: Buffer;
  refCount?: number;
  properties?: Readonly<Record<string, string>>;
  metadata?: Readonly<Map<number, Buffer>>;
}

export function messageFromSnapshot(snapshot: MessageSnapshot): Message {
  const message = Object.create(Message.prototype) as Message;
  const state = message as unknown as {
    _buffer: Buffer;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
  };
  state._buffer = snapshot.data;
  state._refCount = snapshot.refCount ?? 1;
  state._properties = normalizeMessageProperties(snapshot.properties);
  state._metadata = snapshot.metadata ?? EMPTY_METADATA;
  return message;
}

export function messageFromOwnedBuffer(data: Buffer): Message {
  const message = Object.create(Message.prototype) as Message;
  const state = message as unknown as {
    _buffer: Buffer;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
  };
  state._buffer = data;
  state._refCount = 1;
  state._properties = EMPTY_PROPERTIES;
  state._metadata = EMPTY_METADATA;
  return message;
}

export function messageToSnapshot(message: Message): MessageSnapshot {
  const state = message as unknown as {
    _buffer: Buffer;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _metadata: Readonly<Map<number, Buffer>>;
  };
  return {
    data: state._buffer,
    refCount: state._refCount,
    properties: state._properties,
    metadata: state._metadata
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
