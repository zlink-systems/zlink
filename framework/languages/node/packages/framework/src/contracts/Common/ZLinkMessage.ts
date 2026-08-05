import type { Type } from './CoreTypes';
import { ZLinkEncodedPayload } from './ZLinkEncodedPayload';

type ZLinkMessageDecoder = <T>(type?: Type<T>) => T;
const runtimeDecoders = new WeakMap<ZLinkMessage, ZLinkMessageDecoder>();

export class ZLinkMessage<TValue = unknown> {
  private constructor(
    private readonly value: TValue | undefined,
    private readonly encoded: ZLinkEncodedPayload | undefined
  ) {}

  static from<T>(value: T): ZLinkMessage<T> {
    return new ZLinkMessage(value, undefined);
  }

  static fromEncoded(payload: ZLinkEncodedPayload): ZLinkMessage {
    return new ZLinkMessage(undefined, payload);
  }

  decode<T>(type?: Type<T>): T {
    if (this.encoded === undefined) {
      return this.value as T;
    }
    const encoded = this.encoded.data();
    if (encoded.length === 0) {
      return undefined as T;
    }
    const decoder = runtimeDecoders.get(this);
    if (decoder !== undefined) {
      return decoder(type);
    }
    const text = Buffer.from(encoded).toString('utf8');
    try {
      return JSON.parse(text) as T;
    } catch {
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
