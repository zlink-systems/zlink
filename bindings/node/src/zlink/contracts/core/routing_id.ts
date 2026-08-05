// SPDX-License-Identifier: MPL-2.0

import { ConfigError, ConfigResult } from '../errors/errors';

const ROUTING_ID_MAX_LENGTH = 255;
function normalizeRoutingIdBytes(bytes: Buffer | Uint8Array, name: string): Buffer {
  if (!Buffer.isBuffer(bytes) && !(bytes instanceof Uint8Array)) {
    throw new TypeError(`${name} must be a Buffer or Uint8Array`);
  }
  const normalized = Buffer.from(bytes);
  if (normalized.length === 0 || normalized.length > ROUTING_ID_MAX_LENGTH) {
    const error = new ConfigError(ConfigResult.InvalidArgument, 0);
    error.message = `${name} must be 1..${ROUTING_ID_MAX_LENGTH} bytes`;
    throw error;
  }
  return normalized;
}

function normalizeRoutingIdHex(value: string): Buffer {
  if (typeof value !== 'string') {
    throw new TypeError('value must be a string');
  }
  if (value.length === 0 || value.length % 2 !== 0 || !/^[0-9a-fA-F]+$/.test(value)) {
    const error = new ConfigError(ConfigResult.InvalidArgument, 0);
    error.message = 'value must be a non-empty even-length hex string';
    throw error;
  }
  if (value.length > ROUTING_ID_MAX_LENGTH * 2) {
    const error = new ConfigError(ConfigResult.InvalidArgument, 0);
    error.message = `value must decode to at most ${ROUTING_ID_MAX_LENGTH} bytes`;
    throw error;
  }
  return normalizeRoutingIdBytes(Buffer.from(value, 'hex'), 'value');
}

function normalizeRoutingIdValue(value: string | Buffer | Uint8Array | number): Buffer {
  if (typeof value === 'string') {
    return normalizeRoutingIdBytes(Buffer.from(value, 'utf8'), 'value');
  }
  if (typeof value === 'number') {
    if (!Number.isInteger(value) || value < 0 || value > 0xFFFF_FFFF) {
      throw new RangeError('routing id uint32 value must be in range 0..4294967295');
    }
    const buffer = Buffer.allocUnsafe(4);
    buffer.writeUInt32BE(value >>> 0, 0);
    return buffer;
  }
  return normalizeRoutingIdBytes(value, 'value');
}

function tryPrintableUtf8(bytes: Buffer): string | null {
  const text = bytes.toString('utf8');
  if (!Buffer.from(text, 'utf8').equals(bytes)) {
    return null;
  }
  return /[\u0000-\u001f\u007f-\u009f]/u.test(text) ? null : text;
}

function uuidString(bytes: Buffer): string {
  const hex = bytes.toString('hex');
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

/** An opaque identifier for a messaging peer or route, 1 to 255 bytes long. */
const ROUTING_ID_CONSTRUCTOR_TOKEN = Symbol('RoutingId.constructor');

export class RoutingId {
  private readonly _bytes: Buffer;

  private constructor(bytes: Buffer, token: symbol) {
    if (token !== ROUTING_ID_CONSTRUCTOR_TOKEN) {
      throw new TypeError('RoutingId values are created by RoutingId.from or RoutingId.fromHex');
    }
    this._bytes = bytes;
    Object.freeze(this);
  }

  /**
   * Create a routing id from a string (its UTF-8 bytes), a number (a 4-byte
   * big-endian uint32), or a Buffer/Uint8Array (copied). Throws when the byte
   * length is not 1..255.
   */
  static from(value: string | Buffer | Uint8Array | number): RoutingId {
    return new RoutingId(normalizeRoutingIdValue(value), ROUTING_ID_CONSTRUCTOR_TOKEN);
  }

  /**
   * Create a routing id by decoding `value` as a hex string (non-empty, even
   * length, up to 510 digits for 255 bytes).
   */
  static fromHex(value: string): RoutingId {
    return new RoutingId(normalizeRoutingIdHex(value), ROUTING_ID_CONSTRUCTOR_TOKEN);
  }

  /** Return a copy of the routing id bytes. */
  toBytes(): Buffer {
    return Buffer.from(this._bytes);
  }

  /** The length of the routing id in bytes. */
  get size(): number {
    return this._bytes.length;
  }

  /** Return true when `other` has identical bytes. */
  equals(other: RoutingId): boolean {
    return other instanceof RoutingId && this._bytes.equals(other._bytes);
  }

  /** Return the routing id as a lowercase hex string. */
  toHex(): string {
    return this._bytes.toString('hex');
  }

  /**
   * Return a human-readable form: printable UTF-8 text when possible, otherwise
   * a uint, a UUID, or a `hex:` fallback.
   */
  toString(): string {
    const utf8 = tryPrintableUtf8(this._bytes);
    if (utf8 !== null) {
      return utf8;
    }
    if (this._bytes.length === 4) {
      return this._bytes.readUInt32BE(0).toString(10);
    }
    if (this._bytes.length === 16) {
      return uuidString(this._bytes);
    }
    return `hex:${this.toHex()}`;
  }
}
