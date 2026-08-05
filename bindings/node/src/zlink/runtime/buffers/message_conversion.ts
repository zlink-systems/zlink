// SPDX-License-Identifier: MPL-2.0

import {
  Message,
  type MessageLike
} from '../../contracts';
import {
  messageFromOwnedBuffer,
  messageToSnapshot,
  type MessageSnapshot
} from '../messaging/message_snapshot';

function normalizeBufferLike(value: MessageLike, label = 'value'): Buffer {
  if (Buffer.isBuffer(value)) return value;
  if (value instanceof Uint8Array) {
    return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }
  if (typeof value === 'string') return Buffer.from(value);
  throw new TypeError(`${label} must be Buffer, Uint8Array, or string`);
}

export function toMessageParts(parts: readonly MessageLike[]): Array<Buffer | MessageSnapshot> {
  return parts.map((part, index) =>
    part instanceof Message ? messageToSnapshot(part) : normalizeBufferLike(part, `parts[${index}]`)
  );
}

export function normalizeOperationPayload(parts: MessageLike | readonly MessageLike[]): Buffer | MessageSnapshot | Array<Buffer | MessageSnapshot> {
  if (!Array.isArray(parts)) {
    const scalar = parts as MessageLike;
    return scalar instanceof Message ? messageToSnapshot(scalar) : normalizeBufferLike(scalar, 'message');
  }
  if (parts.length === 1) {
    const part = parts[0];
    return part instanceof Message ? messageToSnapshot(part) : normalizeBufferLike(part, 'message');
  }
  return toMessageParts(parts);
}

export { normalizeOperationPayload as normalizeMessageLikePayload };

export function messageFromNativeBuffer(buffer: Buffer | null | undefined): Message {
  // HOT PATH: native callbacks already transfer payload ownership to a JS
  // Buffer. Build the public Message facade directly so STREAM callbacks
  // do not allocate an intermediate snapshot object for every received part.
  return messageFromOwnedBuffer(buffer ?? Buffer.alloc(0));
}
