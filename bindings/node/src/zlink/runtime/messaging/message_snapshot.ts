// SPDX-License-Identifier: MPL-2.0

import {
  Message,
  acquireMessageWrapper,
  canShareNativeMessage
} from '../../contracts/messaging/message';

export interface MessageSnapshot {
  data?: Buffer;
  nativeMessage?: unknown;
  refCount?: number;
  properties?: Readonly<Record<string, string>>;
  metadata?: Readonly<Map<number, Buffer>>;
}

export function messageFromSnapshot(snapshot: MessageSnapshot): Message {
  const message = acquireMessageWrapper(
    snapshot.data,
    snapshot.refCount ?? 1,
    snapshot.properties,
    snapshot.nativeMessage,
    snapshot.metadata
  );
  return message;
}

export function messageFromOwnedBuffer(data: Buffer, nativeMessage?: unknown): Message {
  return acquireMessageWrapper(data, 1, undefined, nativeMessage);
}

export function messageFromNativeFrame(
  nativeMessage: unknown,
  properties?: Readonly<Record<string, string>>
): Message {
  return acquireMessageWrapper(undefined, 1, properties, nativeMessage);
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
