// SPDX-License-Identifier: MPL-2.0

import {
  Message,
  type MessageLike
} from '../../contracts';
import {
  messageFromOwnedBuffer,
  messageToNativeValue,
  messageToSnapshot,
  type MessageSnapshot,
} from '../messaging/message_snapshot';

function normalizeBufferLike(value: MessageLike, label: string | number = 'value'): Buffer {
  if (Buffer.isBuffer(value)) return value;
  if (value instanceof Uint8Array) {
    return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }
  if (typeof value === 'string') return Buffer.from(value);
  throw new TypeError(`${typeof label === 'number' ? `parts[${label}]` : label} must be Buffer, Uint8Array, or string`);
}

export function toMessageParts(parts: readonly MessageLike[]): unknown[] {
  let converted: unknown[] | undefined;
  for (let index = 0; index < parts.length; ++index) {
    const part = parts[index];
    const normalized = part instanceof Message
      ? messageToNativeValue(part) : normalizeBufferLike(part, index);
    if (converted === undefined && normalized !== part) {
      converted = parts.slice(0, index);
    }
    converted?.push(normalized);
  }
  // Native submit only reads the list and copies Buffer bytes synchronously.
  // Allocate a replacement list only when an element actually needs conversion.
  return converted ?? parts as unknown[];
}

export function normalizeOperationPayload(parts: MessageLike | readonly MessageLike[]): unknown {
  if (!Array.isArray(parts)) {
    const scalar = parts as MessageLike;
    return scalar instanceof Message ? messageToNativeValue(scalar) : normalizeBufferLike(scalar, 'message');
  }
  if (parts.length === 1) {
    const part = parts[0];
    return part instanceof Message ? messageToNativeValue(part) : normalizeBufferLike(part, 'message');
  }
  return toMessageParts(parts);
}

export function normalizeMessageLikePayload(
  parts: MessageLike | readonly MessageLike[]
): Buffer | MessageSnapshot | Array<Buffer | MessageSnapshot> {
  if (!Array.isArray(parts)) {
    const scalar = parts as MessageLike;
    return scalar instanceof Message
      ? messageToSnapshot(scalar)
      : normalizeBufferLike(scalar, 'message');
  }
  const converted = new Array<Buffer | MessageSnapshot>(parts.length);
  for (let index = 0; index < parts.length; ++index) {
    const part = parts[index];
    converted[index] = part instanceof Message
      ? messageToSnapshot(part)
      : normalizeBufferLike(part, index);
  }
  return converted;
}

export function messageFromNativeBuffer(buffer: Buffer | null | undefined): Message {
  // HOT PATH: the addon already transfers payload ownership to a JS Buffer.
  return messageFromOwnedBuffer(buffer ?? Buffer.alloc(0));
}
