// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../core/buffer_like';

/** Reserved native metadata key range; metadata lookup is not implemented yet. */
export const METADATA_KEY_USER_MIN = 0x0100;
/** Reserved native metadata value limit; metadata lookup is not implemented yet. */
export const METADATA_VALUE_MAX = 65535;

const EMPTY_PROPERTIES: Readonly<Record<string, string>> = Object.freeze({});
const EMPTY_BUFFER = Buffer.alloc(0);
const writableViews = new WeakSet<Message>();

/** @internal */
export interface MessageNativeOperations {
  allocate(size: number): { data: Buffer; nativeMessage: unknown };
  close(nativeMessage: unknown): void;
  data(nativeMessage: unknown): Buffer;
  fromBuffer(data: Buffer): { data: Buffer; nativeMessage: unknown };
  size(nativeMessage: unknown): number;
}

let nativeOperations: MessageNativeOperations | undefined;

/** @internal Configure the runtime operations hidden behind the Message contract. */
export function configureMessageNativeOperations(operations: MessageNativeOperations): void {
  nativeOperations = operations;
}

function requireMessageNativeOperations(): MessageNativeOperations {
  if (!nativeOperations) {
    throw new Error('Message native operations are not configured');
  }
  return nativeOperations;
}

function normalizeMessageProperties(
  properties?: Readonly<Record<string, string>>
): Readonly<Record<string, string>> {
  if (!properties) {
    return EMPTY_PROPERTIES;
  }
  return Object.isFrozen(properties) ? properties : Object.freeze(properties);
}

function normalizeBufferLike(value: BufferLike, label = 'value'): Buffer {
  if (Buffer.isBuffer(value)) {
    return value;
  }
  if (value instanceof Uint8Array) {
    return Buffer.from(value.buffer, value.byteOffset, value.byteLength);
  }
  if (typeof value === 'string') {
    return Buffer.from(value);
  }
  throw new TypeError(`${label} must be Buffer, Uint8Array, or string`);
}

/**
 * A message payload backed by native storage. A successful submit consumes the
 * message and leaves it empty; `close` releases an unsubmitted message early.
 */
export class Message {
  private _buffer!: Buffer;
  private _refCount!: number;
  private _properties!: Readonly<Record<string, string>>;
  /** Opaque native frame that owns _buffer's storage. */
  private _nativeMessage?: unknown;

  private constructor(data: BufferLike) {
    const nativeMessage = requireMessageNativeOperations().fromBuffer(
      normalizeBufferLike(data, 'data')
    );
    this.initialize(nativeMessage.data, 1, undefined, nativeMessage.nativeMessage);
  }

  private initialize(
    buffer: Buffer,
    refCount = 1,
    properties?: Readonly<Record<string, string>>,
    nativeMessage?: unknown
  ): void {
    this._buffer = buffer;
    this._refCount = refCount | 0;
    this._properties = normalizeMessageProperties(properties);
    this._nativeMessage = nativeMessage;
  }

  /**
   * Create a message holding an independent copy of `buffer` (a buffer-like
   * value or another {@link Message}); the source may be freely reused
   * afterward.
   */
  static from(buffer: BufferLike | Message): Message {
    if (buffer instanceof Message) {
      return new Message(buffer.ensureBuffer());
    }
    return new Message(buffer);
  }

  /** Allocate a message with `size` bytes of writable payload storage. */
  static allocate(size: number): Message {
    if (!Number.isSafeInteger(size) || size < 0) {
      throw new RangeError('size must be a non-negative safe integer');
    }
    const nativeMessage = requireMessageNativeOperations().allocate(size);
    const message = Object.create(Message.prototype) as Message;
    message.initialize(nativeMessage.data, 1, undefined, nativeMessage.nativeMessage);
    return message;
  }

  /** Return the payload as a Buffer backed by this message's storage. */
  data(): Buffer {
    // The returned Buffer is writable. Later send must therefore make a
    // payload copy instead of sharing native storage with an in-flight send.
    writableViews.add(this);
    return this.ensureBuffer();
  }

  /** Return a new Buffer copying the payload. */
  toBytes(): Buffer {
    return Buffer.from(this.ensureBuffer());
  }

  /** Return a new message holding an independent copy of this payload. */
  copy(): Message {
    return Message.from(this);
  }

  /** Return the payload size in bytes. */
  size(): number {
    if (this._buffer !== undefined) {
      return this._buffer.length;
    }
    if (this._nativeMessage !== undefined) {
      return requireMessageNativeOperations().size(this._nativeMessage);
    }
    return 0;
  }

  /** Return true when the payload is empty. */
  isEmpty(): boolean {
    return this.size() === 0;
  }

  /**
   * Copy the payload (or `length` bytes from `sourceOffset`) into `destination`
   * at `destinationOffset`; return the number of bytes written. Throws
   * {@link RangeError} when the range is out of bounds.
   */
  copyTo(
    destination: Buffer | Uint8Array,
    sourceOffset = 0,
    destinationOffset = 0,
    length = this.size() - sourceOffset
  ): number {
    if (!Buffer.isBuffer(destination) && !(destination instanceof Uint8Array)) {
      throw new TypeError('destination must be a Buffer or Uint8Array');
    }
    if (
      !Number.isSafeInteger(sourceOffset)
      || sourceOffset < 0
      || !Number.isSafeInteger(destinationOffset)
      || destinationOffset < 0
      || !Number.isSafeInteger(length)
      || length < 0
      || sourceOffset + length > this.size()
      || destinationOffset + length > destination.byteLength
    ) {
      throw new RangeError('copy range is out of bounds');
    }
    const target = Buffer.isBuffer(destination)
      ? destination
      : Buffer.from(destination.buffer, destination.byteOffset, destination.byteLength);
    return this.ensureBuffer().copy(
      target,
      destinationOffset,
      sourceOffset,
      sourceOffset + length
    );
  }

  /**
   * Copy the payload into `destination` when it fits; return true on success,
   * or false when `destination` is too small.
   */
  tryCopyTo(destination: Buffer | Uint8Array): boolean {
    if (destination.byteLength < this.size()) {
      return false;
    }
    this.copyTo(destination);
    return true;
  }

  /** Decode the payload as text using `encoding`. */
  getString(encoding: BufferEncoding = 'utf8'): string {
    return this.ensureBuffer().toString(encoding);
  }

  /**
   * Return the native message property `name`, or null when it is absent.
   * Native message metadata is reserved in the current binding and is not
   * populated yet, so public factory and receive paths currently return null.
   */
  getProperty(name: string): string | null {
    if (typeof name !== 'string') {
      throw new TypeError('property name must be a string');
    }
    return Object.prototype.hasOwnProperty.call(this._properties, name)
      ? this._properties[name]
      : null;
  }

  /** Return the native payload reference count (a diagnostic only). */
  refCount(): number {
    return this._refCount;
  }

  /**
   * Release native storage and leave this message empty. Repeated calls have
   * no effect.
   */
  close(): void {
    if (this._nativeMessage !== undefined) {
      requireMessageNativeOperations().close(this._nativeMessage);
    }
    this._buffer = EMPTY_BUFFER;
    this._refCount = 0;
    this._properties = EMPTY_PROPERTIES;
    this._nativeMessage = undefined;
  }

  /** Return the payload decoded as a UTF-8 string. */
  toString(): string {
    return this.getString();
  }

  private ensureBuffer(): Buffer {
    if (this._buffer !== undefined) {
      return this._buffer;
    }
    if (this._nativeMessage === undefined) {
      return EMPTY_BUFFER;
    }
    this._buffer = requireMessageNativeOperations().data(this._nativeMessage);
    return this._buffer;
  }
}

/** @internal */
export function canShareNativeMessage(message: Message): boolean {
  const state = message as unknown as { _nativeMessage?: unknown };
  return state._nativeMessage !== undefined && !writableViews.has(message);
}

/** @internal Mark a successfully submitted message empty without another native call. */
export function consumeSubmittedMessage(message: Message): void {
  if (!canShareNativeMessage(message)) {
    message.close();
    return;
  }
  const state = message as unknown as {
    _buffer: Buffer;
    _refCount: number;
    _properties: Readonly<Record<string, string>>;
    _nativeMessage?: unknown;
  };
  state._buffer = EMPTY_BUFFER;
  state._refCount = 0;
  state._properties = EMPTY_PROPERTIES;
  state._nativeMessage = undefined;
}

/** @internal Refill caller-provided receive storage with the next native frame. */
export type MessageLike = Message | BufferLike;
