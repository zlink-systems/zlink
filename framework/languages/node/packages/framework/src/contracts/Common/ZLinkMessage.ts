import type { Type } from './CoreTypes';
import { ZLinkEncodedPayload } from './ZLinkEncodedPayload';

type ZLinkMessageDecoder = <T>(type?: Type<T>) => T;
const runtimeDecoders = new WeakMap<ZLinkMessage, ZLinkMessageDecoder>();
const runtimeDecodedValues = new WeakMap<ZLinkMessage, unknown>();
const runtimeDecodeFailures = new WeakMap<ZLinkMessage, unknown>();
const runtimeDecoded = new WeakSet<ZLinkMessage>();
const declaredMessageTypes = new WeakMap<ZLinkMessage, Type>();

export class ZLinkMessage<TValue = unknown> {
  private constructor(
    private readonly value: TValue | undefined,
    private readonly encoded: ZLinkEncodedPayload | undefined
  ) {}

  static from<T>(value: T, declaredType?: Type<T>): ZLinkMessage<T> {
    const message = new ZLinkMessage(value, undefined);
    if (declaredType !== undefined) {
      declaredMessageTypes.set(message, declaredType);
    }
    return message;
  }

  static fromEncoded(payload: ZLinkEncodedPayload): ZLinkMessage {
    return new ZLinkMessage(undefined, payload);
  }

  decode<T>(type?: Type<T>): T {
    if (this.encoded === undefined) {
      return this.value as T;
    }
    if (runtimeDecoded.has(this)) {
      if (runtimeDecodeFailures.has(this)) {
        throw runtimeDecodeFailures.get(this);
      }
      return runtimeDecodedValues.get(this) as T;
    }
    if (this.encoded.isEmpty()) {
      runtimeDecoded.add(this);
      runtimeDecodedValues.set(this, undefined);
      return undefined as T;
    }
    const decoder = runtimeDecoders.get(this);
    if (decoder !== undefined) {
      try {
        const value = decoder(type);
        runtimeDecoded.add(this);
        runtimeDecodedValues.set(this, value);
        return value;
      } catch (error) {
        runtimeDecoded.add(this);
        runtimeDecodeFailures.set(this, error);
        throw error;
      }
    }
    const text = this.encoded.getString('utf8');
    try {
      const value = materializeZLinkMessageValue(JSON.parse(text) as T, type);
      runtimeDecoded.add(this);
      runtimeDecodedValues.set(this, value);
      return value;
    } catch {
      runtimeDecoded.add(this);
      runtimeDecodedValues.set(this, text);
      return text as T;
    }
  }

  toEncodedPayload(): ZLinkEncodedPayload {
    if (this.encoded !== undefined) {
      return this.encoded;
    }
    if (Buffer.isBuffer(this.value) || this.value instanceof Uint8Array) {
      return ZLinkEncodedPayload.from(this.value);
    }
    return ZLinkEncodedPayload.from(Buffer.from(JSON.stringify(this.value ?? null)));
  }

  isEncoded(): boolean {
    return this.encoded !== undefined;
  }
}

/** Internal runtime factory that preserves lazy typed decoding without exposing serializer selection. */
export function createZLinkMessageFromEncoded(
  payload: ZLinkEncodedPayload,
  decoder: ZLinkMessageDecoder
): ZLinkMessage {
  const message = ZLinkMessage.fromEncoded(payload);
  runtimeDecoders.set(message, decoder);
  return message;
}

export function isZLinkMessage(value: unknown): value is ZLinkMessage {
  return value instanceof ZLinkMessage;
}

/** Internal runtime accessor for the type explicitly retained by `ZLinkMessage.from`. */
export function readZLinkMessageDeclaredType(message: ZLinkMessage): Type | undefined {
  return declaredMessageTypes.get(message);
}

/**
 * Reattaches a caller-provided DTO prototype without invoking its constructor.
 * The wire decoder owns validation and conversion; this helper only preserves
 * the runtime type requested by `ZLinkMessage.decode(type)`.
 */
export function materializeZLinkMessageValue<T>(value: T, type?: Type<T>): T {
  if (
    type === undefined
    || (type as unknown) === Object
    || value === null
    || typeof value !== 'object'
    || Array.isArray(value)
  ) {
    return value;
  }

  const prototype = (type as Type<T> & { readonly prototype?: object }).prototype;
  if (prototype === undefined || prototype === Object.prototype) {
    return value;
  }

  const materialized = Object.create(prototype) as Record<string, unknown>;
  for (const [key, item] of Object.entries(value as Record<string, unknown>)) {
    Object.defineProperty(materialized, key, {
      configurable: true,
      enumerable: true,
      value: item,
      writable: true
    });
  }
  return materialized as T;
}
