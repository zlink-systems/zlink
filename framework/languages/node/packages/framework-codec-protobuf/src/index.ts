import type {
  ZlinkStreamEncodedPayload,
  ZlinkStreamPayloadCodec
} from '@zlink-systems/stream-connector';
import { ZlinkStreamCodec, ZlinkStreamContentType } from '@zlink-systems/stream-wire';

export const ZLINK_PROTOBUF_CONTENT_TYPE = ZlinkStreamContentType.Protobuf;
export const zlinkStreamProtobufCodecName = 'protobuf';

export interface ProtobufType<T> {
  encode(message: T): { finish(): Uint8Array };
  decode(reader: Uint8Array): T;
}

export interface ProtobufEnvelopeCodecOptions {
  encode(payload: unknown, context?: ProtobufEncodeContext | Function): ZlinkStreamEncodedPayload;
  decode<TPayload = unknown>(payload: ZlinkStreamEncodedPayload, messageType?: Function): TPayload;
}

export type ProtobufMessageDirection = 'Request' | 'Response' | 'Error' | 'Send';

export interface ProtobufEncodeContext {
  readonly messageType?: Function;
  readonly packetName?: string;
  readonly direction: ProtobufMessageDirection;
}

export type ZlinkStreamProtobufEnvelopeCodec = ZlinkStreamPayloadCodec & {
  encode(payload: unknown, context?: ProtobufEncodeContext | Function): ZlinkStreamEncodedPayload;
};

export function createZlinkStreamProtobufCodec<T>(type: ProtobufType<T>): ZlinkStreamPayloadCodec {
  return {
    encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload {
      const payloadType = (messageType ?? inferMessageType(payload)) as
        (Function & Partial<ProtobufType<T>>) | undefined;
      return toProto(
        payload as T,
        typeof payloadType?.encode === 'function' ? (payloadType as ProtobufType<T>) : type,
        payloadType
      );
    },
    decode<TPayload = unknown>(
      payload: ZlinkStreamEncodedPayload,
      messageType?: Function
    ): TPayload {
      return fromProto(payload, (messageType ?? type) as unknown as ProtobufType<TPayload>);
    }
  };
}

export function createZlinkStreamProtobufEnvelopeCodec(
  options: ProtobufEnvelopeCodecOptions
): ZlinkStreamProtobufEnvelopeCodec {
  return {
    encode(
      payload: unknown,
      context?: ProtobufEncodeContext | Function
    ): ZlinkStreamEncodedPayload {
      return options.encode(payload, context);
    },
    decode<TPayload = unknown>(
      payload: ZlinkStreamEncodedPayload,
      messageType?: Function
    ): TPayload {
      return options.decode<TPayload>(payload, messageType);
    }
  };
}

export function toProto<T>(
  value: T,
  type: ProtobufType<T>,
  messageType?: Function
): ZlinkStreamEncodedPayload {
  return {
    codec: ZlinkStreamCodec.Protobuf,
    payload: type.encode(value).finish(),
    messageType: messageType ?? inferMessageType(value)
  };
}

export function fromProto<T>(payload: ZlinkStreamEncodedPayload, type: ProtobufType<T>): T {
  ensureProtobuf(payload);
  return type.decode(payload.payload);
}

function ensureProtobuf(payload: ZlinkStreamEncodedPayload): void {
  if (payload.codec !== ZlinkStreamCodec.Protobuf) {
    throw new Error(`Stream payload codec is ${payload.codec}, not Protobuf.`);
  }
}

function inferMessageType(value: unknown): Function | undefined {
  if (value === null || value === undefined) {
    return undefined;
  }
  const constructor = (value as { constructor?: Function }).constructor;
  return constructor === Object ? undefined : constructor;
}
