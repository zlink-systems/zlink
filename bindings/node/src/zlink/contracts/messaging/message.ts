// SPDX-License-Identifier: MPL-2.0

import type { BufferLike } from '../core/buffer_like';

/** Reserved native metadata key range; metadata lookup is not implemented yet. */
export const METADATA_KEY_USER_MIN = 0x0100;
/** Reserved native metadata value limit; metadata lookup is not implemented yet. */
export const METADATA_VALUE_MAX = 65535;

const EMPTY_PROPERTIES: Readonly<Record<string, string>> = Object.freeze({});
const EMPTY_METADATA: Readonly<Map<number, Buffer>> = Object.freeze(new Map<number, Buffer>());
const EMPTY_BUFFER = Buffer.alloc(0);
const MESSAGE_WRAPPER_POOL_CAPACITY = 64;
const messageWrapperPool: Message[] = [];

type MutableMessageState = {
  _buffer: Buffer | undefined;
  _refCount: number;
  _properties: Readonly<Record<string, string>>;
  _metadata: Readonly<Map<number, Buffer>>;
  _nativeMessage?: unknown;
  _nativeReadOnly: boolean;
  _released: boolean;
};

/** @internal */
export interface MessageNativeOperations {
  allocate(size: number): { data?: Buffer; nativeMessage: unknown };
  close(nativeMessage: unknown): void;
  data(nativeMessage: unknown): Buffer;
  copyData(nativeMessage: unknown): Buffer;
  fromBuffer(data: Buffer): { data?: Buffer; nativeMessage: unknown };
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
 * A message payload owned by this wrapper. The payload can use a JavaScript
 * Buffer or native storage. A successful submit consumes the message; `close`
 * releases it. Do not use a reference after either terminal action because the
 * runtime may reuse the returned wrapper identity.
 */
export class Message {
  private _buffer!: Buffer | undefined;
  private _refCount!: number;
  private _properties!: Readonly<Record<string, string>>;
  /** Opaque owner when this message uses native storage. */
  private _nativeMessage?: unknown;
  /** True when a received frame remains movable until data() exposes it. */
  private _nativeReadOnly!: boolean;
  /** True after ownership has ended and this facade has entered the pool. */
  private _released!: boolean;

  /**
   * Create a message holding an independent copy of `buffer` (a buffer-like
   * value or another {@link Message}); the source may be freely reused
   * afterward.
   */
  static from(buffer: BufferLike | Message): Message {
    const source = buffer instanceof Message
      ? buffer.ensureBuffer()
      : normalizeBufferLike(buffer, 'data');
    const nativeMessage = requireMessageNativeOperations().fromBuffer(source);
    return acquireMessageWrapper(
      nativeMessage.data,
      1,
      undefined,
      nativeMessage.nativeMessage
    );
  }

  /** Allocate a message with `size` bytes of writable payload storage. */
  static allocate(size: number): Message {
    if (!Number.isSafeInteger(size) || size < 0) {
      throw new RangeError('size must be a non-negative safe integer');
    }
    const nativeMessage = requireMessageNativeOperations().allocate(size);
    return acquireMessageWrapper(
      nativeMessage.data,
      1,
      undefined,
      nativeMessage.nativeMessage
    );
  }

  /** Return the payload as a Buffer backed by this message's storage. */
  data(): Buffer {
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

  /** Release native storage and return this wrapper; do not use it afterward. */
  close(): void {
    if (this._released) {
      return;
    }
    if (this._nativeMessage !== undefined) {
      requireMessageNativeOperations().close(this._nativeMessage);
    }
    releaseMessageWrapper(this);
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
    const nativeMessage = this._nativeMessage;
    const operations = requireMessageNativeOperations();
    if (this._nativeReadOnly) {
      // Router receive storage is movable until JavaScript observes it. Once
      // exposed, materialize a managed Buffer and release the native frame.
      this._buffer = operations.copyData(nativeMessage);
      operations.close(nativeMessage);
      this._nativeMessage = undefined;
      this._nativeReadOnly = false;
      return this._buffer;
    }
    this._buffer = operations.data(nativeMessage);
    return this._buffer;
  }
}

/** @internal Acquire a Message facade for runtime-owned receive state. */
export function acquireMessageWrapper(
  buffer: Buffer | undefined,
  refCount = 1,
  properties?: Readonly<Record<string, string>>,
  nativeMessage?: unknown,
  metadata?: Readonly<Map<number, Buffer>>,
  nativeReadOnly = false
): Message {
  const message = messageWrapperPool.pop()
    ?? Object.create(Message.prototype) as Message;
  const state = message as unknown as MutableMessageState;
  state._buffer = buffer;
  state._refCount = refCount | 0;
  state._properties = normalizeMessageProperties(properties);
  state._metadata = metadata ?? EMPTY_METADATA;
  state._nativeMessage = nativeMessage;
  state._nativeReadOnly = nativeReadOnly;
  state._released = false;
  return message;
}

function releaseMessageWrapper(message: Message): void {
  const state = message as unknown as MutableMessageState;
  if (state._released) {
    return;
  }
  state._buffer = EMPTY_BUFFER;
  state._refCount = 0;
  state._properties = EMPTY_PROPERTIES;
  state._metadata = EMPTY_METADATA;
  state._nativeMessage = undefined;
  state._nativeReadOnly = false;
  state._released = true;
  if (messageWrapperPool.length < MESSAGE_WRAPPER_POOL_CAPACITY) {
    messageWrapperPool.push(message);
  }
}

function markMessageConsumed(message: Message): void {
  const state = message as unknown as MutableMessageState;
  state._buffer = EMPTY_BUFFER;
  state._refCount = 0;
  state._properties = EMPTY_PROPERTIES;
  state._metadata = EMPTY_METADATA;
  state._nativeMessage = undefined;
  state._nativeReadOnly = false;
}

/** @internal */
export function canShareNativeMessage(message: Message): boolean {
  const state = message as unknown as { _nativeMessage?: unknown };
  return state._nativeMessage !== undefined;
}

/** @internal Mark a successfully submitted message empty without another native call. */
export function consumeSubmittedMessage(message: Message): void {
  const state = message as unknown as MutableMessageState;
  if (state._nativeMessage !== undefined) {
    requireMessageNativeOperations().close(state._nativeMessage);
  }
  markMessageConsumed(message);
}

/** @internal Refill caller-provided receive storage with the next native frame. */
export type MessageLike = Message | BufferLike;
