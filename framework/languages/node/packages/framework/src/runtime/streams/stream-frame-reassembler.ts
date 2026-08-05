// SPDX-License-Identifier: MPL-2.0

import { constants as bufferConstants } from 'node:buffer';

export interface ZLinkAssembledStreamFrame {
  readonly header: Buffer;
  readonly payload: Buffer;
}

export type ZLinkStreamFrameReadResult =
  | { readonly kind: 'incomplete' }
  | { readonly kind: 'malformed'; readonly error: Error }
  | { readonly kind: 'frame'; readonly frame: ZLinkAssembledStreamFrame };

const STREAM_FRAME_PREFIX_BYTES = 6;
const EMPTY_BUFFER = Buffer.alloc(0);

/**
 * Retains raw STREAM bytes until one complete length-prefixed frame is
 * available. A complete frame in an otherwise empty raw part is exposed as
 * Buffer views, so the common receive path does not copy it. Bytes that span
 * raw parts are copied into reusable storage until the frame is complete.
 */
export class ZLinkStreamFrameReassembler {
  private storage = EMPTY_BUFFER;
  private start = 0;
  private end = 0;
  private directView: Buffer | undefined;

  get hasPendingBytes(): boolean {
    return this.directView !== undefined || this.end > this.start;
  }

  append(bytes: Uint8Array): void {
    if (bytes.byteLength === 0) {
      return;
    }
    if (this.directView !== undefined) {
      const pending = this.directView;
      this.directView = undefined;
      this.ensureCapacity(pending.length + bytes.byteLength);
      pending.copy(this.storage, this.end);
      this.end += pending.length;
      Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
        .copy(this.storage, this.end);
      this.end += bytes.byteLength;
      return;
    }
    if (this.start === this.end) {
      this.start = 0;
      this.end = 0;
      this.directView = Buffer.isBuffer(bytes)
        ? bytes
        : Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      return;
    }
    this.ensureCapacity(bytes.byteLength);
    Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength)
      .copy(this.storage, this.end);
    this.end += bytes.byteLength;
  }

  next(maxMessageSize = 0): ZLinkStreamFrameReadResult {
    if (this.directView !== undefined) {
      return this.nextDirect(this.directView, maxMessageSize);
    }
    const available = this.end - this.start;
    if (available < STREAM_FRAME_PREFIX_BYTES) {
      return { kind: 'incomplete' };
    }

    const headerSize = this.storage.readUInt16BE(this.start);
    const payloadSize = this.storage.readUInt32BE(this.start + 2);
    const totalSize = STREAM_FRAME_PREFIX_BYTES + headerSize + payloadSize;
    if (
      (maxMessageSize > 0 && (
        headerSize > maxMessageSize
        || payloadSize > maxMessageSize
        || headerSize > maxMessageSize - payloadSize
      ))
      || totalSize > bufferConstants.MAX_LENGTH
    ) {
      return {
        kind: 'malformed',
        error: new Error('STREAM frame exceeds MaxMessageSize.')
      };
    }
    if (available < totalSize) {
      return { kind: 'incomplete' };
    }

    const storage = this.storage;
    const frameStart = this.start + STREAM_FRAME_PREFIX_BYTES;
    const payloadStart = frameStart + headerSize;
    const header = storage.subarray(frameStart, payloadStart);
    const payload = storage.subarray(payloadStart, payloadStart + payloadSize);
    const remainingStart = this.start + totalSize;
    this.directView = remainingStart < this.end
      ? storage.subarray(remainingStart, this.end)
      : undefined;
    // Transfer the backing buffer to the returned frame. Any remaining bytes
    // stay as a view over that transferred buffer and are copied only if a
    // later append needs reusable storage. This avoids a second allocation and
    // copy for a segmented frame that has already been assembled.
    this.storage = EMPTY_BUFFER;
    this.start = 0;
    this.end = 0;
    return { kind: 'frame', frame: { header, payload } };
  }

  clear(): void {
    this.directView = undefined;
    this.storage = EMPTY_BUFFER;
    this.start = 0;
    this.end = 0;
  }

  private nextDirect(bytes: Buffer, maxMessageSize: number): ZLinkStreamFrameReadResult {
    if (bytes.length < STREAM_FRAME_PREFIX_BYTES) {
      return { kind: 'incomplete' };
    }
    const headerSize = (bytes[0] << 8) | bytes[1];
    const payloadSize = (
      (bytes[2] * 0x1000000)
      + (bytes[3] << 16)
      + (bytes[4] << 8)
      + bytes[5]
    );
    const totalSize = STREAM_FRAME_PREFIX_BYTES + headerSize + payloadSize;
    if (
      (maxMessageSize > 0 && (
        headerSize > maxMessageSize
        || payloadSize > maxMessageSize
        || headerSize > maxMessageSize - payloadSize
      ))
      || totalSize > bufferConstants.MAX_LENGTH
    ) {
      return {
        kind: 'malformed',
        error: new Error('STREAM frame exceeds MaxMessageSize.')
      };
    }
    if (bytes.length < totalSize) {
      return { kind: 'incomplete' };
    }
    const header = bytes.subarray(STREAM_FRAME_PREFIX_BYTES, STREAM_FRAME_PREFIX_BYTES + headerSize);
    const payload = bytes.subarray(STREAM_FRAME_PREFIX_BYTES + headerSize, totalSize);
    this.directView = totalSize === bytes.length ? undefined : bytes.subarray(totalSize);
    return { kind: 'frame', frame: { header, payload } };
  }

  private ensureCapacity(additionalBytes: number): void {
    const remaining = this.end - this.start;
    const required = remaining + additionalBytes;
    if (this.end + additionalBytes <= this.storage.length) {
      return;
    }
    if (required <= this.storage.length) {
      this.storage.copy(this.storage, 0, this.start, this.end);
      this.start = 0;
      this.end = remaining;
      return;
    }
    if (required > bufferConstants.MAX_LENGTH) {
      throw new Error('STREAM frame buffer exceeds the runtime buffer limit.');
    }
    const capacity = Math.min(
      bufferConstants.MAX_LENGTH,
      Math.max(required, Math.max(256, this.storage.length * 2))
    );
    const next = Buffer.allocUnsafe(capacity);
    if (remaining > 0) {
      this.storage.copy(next, 0, this.start, this.end);
    }
    this.storage = next;
    this.start = 0;
    this.end = remaining;
  }
}
