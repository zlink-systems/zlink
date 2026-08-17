// SPDX-License-Identifier: MPL-2.0

import { constants as bufferConstants } from 'node:buffer';

export interface ZLinkAssembledStreamFrame {
  readonly header: Buffer;
  readonly payload: Buffer;
  close(): void;
}

export interface ZLinkRetainedStreamOwner {
  close(): void;
}

export type ZLinkStreamFrameReadResult =
  | { readonly kind: 'incomplete' }
  | { readonly kind: 'malformed'; readonly error: Error }
  | { readonly kind: 'frame'; readonly frame: ZLinkAssembledStreamFrame };

const STREAM_FRAME_PREFIX_BYTES = 6;
const EMPTY_BUFFER = Buffer.alloc(0);
const EMPTY_RETAINED_OWNERS: readonly ZLinkRetainedStreamOwnerState[] = [];

class ZLinkRetainedStreamOwnerState {
  private bufferedSegments = 0;
  private frameClaims = 0;
  private closed = false;

  constructor(private readonly owner: ZLinkRetainedStreamOwner) {}

  addBufferedSegment(): void {
    if (this.closed) {
      throw new Error('STREAM retained receive owner is already closed.');
    }
    this.bufferedSegments += 1;
  }

  removeBufferedSegment(): void {
    if (this.bufferedSegments <= 0) {
      throw new Error('STREAM retained segment accounting underflow.');
    }
    this.bufferedSegments -= 1;
    this.tryClose();
  }

  claimFrame(): void {
    if (this.closed) {
      throw new Error('STREAM retained receive owner is already closed.');
    }
    this.frameClaims += 1;
  }

  releaseFrame(): void {
    if (this.frameClaims <= 0) {
      throw new Error('STREAM retained frame accounting underflow.');
    }
    this.frameClaims -= 1;
    this.tryClose();
  }

  closeNow(): void {
    if (this.closed) return;
    this.closed = true;
    this.owner.close();
  }

  private tryClose(): void {
    if (this.bufferedSegments === 0 && this.frameClaims === 0) {
      this.closeNow();
    }
  }
}

interface ZLinkRetainedStreamSegment {
  remainingBytes: number;
  readonly owner?: ZLinkRetainedStreamOwnerState;
}

class ZLinkAssembledStreamFrameImpl implements ZLinkAssembledStreamFrame {
  private released = false;

  constructor(
    readonly header: Buffer,
    readonly payload: Buffer,
    private readonly owners: readonly ZLinkRetainedStreamOwnerState[]
  ) {}

  close(): void {
    if (this.released) return;
    this.released = true;
    for (const owner of this.owners) {
      owner.releaseFrame();
    }
  }
}

export class ZLinkStreamMessageSizeError extends Error {
  readonly code = 'EMSGSIZE';

  constructor() {
    super('STREAM frame exceeds MaxMessageSize.');
    this.name = 'ZLinkStreamMessageSizeError';
  }
}

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
  private retainedSegments: ZLinkRetainedStreamSegment[] = [];
  private retainedHead = 0;

  get hasPendingBytes(): boolean {
    return this.directView !== undefined || this.end > this.start;
  }

  append(bytes: Uint8Array): void {
    this.appendOwned(bytes);
  }

  appendRetained(
    parts: readonly { data(): Uint8Array }[],
    retainedOwner: ZLinkRetainedStreamOwner
  ): void {
    const owner = new ZLinkRetainedStreamOwnerState(retainedOwner);
    let retained = false;
    try {
      for (const part of parts) {
        const bytes = part.data();
        if (bytes.byteLength === 0) continue;
        this.appendOwned(bytes, owner);
        retained = true;
      }
    } catch (error) {
      if (!retained) owner.closeNow();
      throw error;
    }
    if (!retained) owner.closeNow();
  }

  private appendOwned(
    bytes: Uint8Array,
    owner?: ZLinkRetainedStreamOwnerState
  ): void {
    if (bytes.byteLength === 0) {
      return;
    }
    owner?.addBufferedSegment();
    this.retainedSegments.push({
      remainingBytes: bytes.byteLength,
      owner
    });
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
        error: new ZLinkStreamMessageSizeError()
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
    const owners = this.consumeRetained(totalSize);
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
    return { kind: 'frame', frame: new ZLinkAssembledStreamFrameImpl(header, payload, owners) };
  }

  clear(): void {
    this.directView = undefined;
    this.storage = EMPTY_BUFFER;
    this.start = 0;
    this.end = 0;
    while (this.retainedHead < this.retainedSegments.length) {
      this.retainedSegments[this.retainedHead]?.owner?.removeBufferedSegment();
      this.retainedHead += 1;
    }
    this.retainedSegments = [];
    this.retainedHead = 0;
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
        error: new ZLinkStreamMessageSizeError()
      };
    }
    if (bytes.length < totalSize) {
      return { kind: 'incomplete' };
    }
    const header = bytes.subarray(STREAM_FRAME_PREFIX_BYTES, STREAM_FRAME_PREFIX_BYTES + headerSize);
    const payload = bytes.subarray(STREAM_FRAME_PREFIX_BYTES + headerSize, totalSize);
    const owners = this.consumeRetained(totalSize);
    this.directView = totalSize === bytes.length ? undefined : bytes.subarray(totalSize);
    return { kind: 'frame', frame: new ZLinkAssembledStreamFrameImpl(header, payload, owners) };
  }

  private consumeRetained(consumedBytes: number): readonly ZLinkRetainedStreamOwnerState[] {
    let remaining = consumedBytes;
    let owners: ZLinkRetainedStreamOwnerState[] | undefined;
    let lastOwner: ZLinkRetainedStreamOwnerState | undefined;
    while (remaining > 0 && this.retainedHead < this.retainedSegments.length) {
      const segment = this.retainedSegments[this.retainedHead]!;
      const consumed = Math.min(remaining, segment.remainingBytes);
      remaining -= consumed;
      segment.remainingBytes -= consumed;
      // One owner is created per appendRetained call, whose parts are appended
      // synchronously and therefore occupy one contiguous segment run.
      if (segment.owner !== undefined && segment.owner !== lastOwner) {
        segment.owner.claimFrame();
        (owners ??= []).push(segment.owner);
        lastOwner = segment.owner;
      }
      if (segment.remainingBytes === 0) {
        segment.owner?.removeBufferedSegment();
        this.retainedHead += 1;
      }
    }
    if (remaining !== 0) {
      throw new Error('STREAM retained byte accounting is incomplete.');
    }
    if (
      this.retainedHead > 64
      && this.retainedHead * 2 >= this.retainedSegments.length
    ) {
      this.retainedSegments = this.retainedSegments.slice(this.retainedHead);
      this.retainedHead = 0;
    }
    return owners ?? EMPTY_RETAINED_OWNERS;
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
