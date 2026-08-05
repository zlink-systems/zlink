import {
  ZLinkEncodedPayload,
  type ZLinkCodecExtension,
  type ZLinkCodecRegistrar,
  type ZLinkMessageSerializer
} from '@zlink-systems/framework';
import type {
  ZlinkStreamEncodedPayload,
  ZlinkStreamPayloadCodec
} from '@zlink-systems/stream-connector';
import { ZlinkStreamCodec } from '@zlink-systems/stream-wire';
import {
  createDynamicValueProtobufType,
  decodeDynamicValue,
  encodeDynamicValue
} from './dynamic-value-wire';
import {
  createZlinkStreamProtobufEnvelopeCodec,
  type ProtobufEncodeContext,
  type ProtobufEnvelopeCodecOptions,
  ZLINK_PROTOBUF_CONTENT_TYPE
} from './index';

export { ZLINK_PROTOBUF_CONTENT_TYPE } from './index';

export type ZLinkProtobufCodecExtension = ZLinkCodecExtension & ZlinkStreamPayloadCodec;

export type ZLinkProtobufEnvelopeCodecExtension = ZLinkProtobufCodecExtension & {
  encode(payload: unknown, context?: ProtobufEncodeContext | Function): ZlinkStreamEncodedPayload;
};

export function zlinkProtobufCodec(): ZLinkProtobufCodecExtension {
  const type = createDynamicValueProtobufType();
  return {
    register(codecs: ZLinkCodecRegistrar): void {
      codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createProtobufMessageSerializer());
      codecs.addStreamCodec(ZLINK_PROTOBUF_CONTENT_TYPE, this);
    },
    encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload {
      return {
        codec: ZlinkStreamCodec.Protobuf,
        payload: type.encode(payload).finish(),
        messageType
      };
    },
    decode<TPayload = unknown>(payload: ZlinkStreamEncodedPayload): TPayload {
      return type.decode(payload.payload) as TPayload;
    }
  };
}

export function createProtobufMessageSerializer(): ZLinkMessageSerializer {
  return {
    serialize<T>(value: T): ZLinkEncodedPayload {
      return ZLinkEncodedPayload.from(encodeDynamicValue(value));
    },
    deserialize<T>(payload: ZLinkEncodedPayload): T {
      return decodeDynamicValue(Buffer.from(payload.data())) as T;
    }
  };
}

export function createZlinkProtobufEnvelopeCodec(
  options: ProtobufEnvelopeCodecOptions
): ZLinkProtobufEnvelopeCodecExtension {
  const streamCodec = createZlinkStreamProtobufEnvelopeCodec(options);
  return {
    register(codecs: ZLinkCodecRegistrar): void {
      codecs.addSerializer(ZLINK_PROTOBUF_CONTENT_TYPE, createProtobufEnvelopeMessageSerializer(options));
      codecs.addStreamCodec(ZLINK_PROTOBUF_CONTENT_TYPE, this);
    },
    encode: streamCodec.encode,
    decode: streamCodec.decode
  };
}

export function createProtobufEnvelopeMessageSerializer(
  options: ProtobufEnvelopeCodecOptions
): ZLinkMessageSerializer {
  return {
    serialize<T>(value: T): ZLinkEncodedPayload {
      const encoded = options.encode(value, inferMessageType(value));
      return ZLinkEncodedPayload.from(encoded.payload);
    },
    deserialize<T>(payload: ZLinkEncodedPayload): T {
      return options.decode<T>({
        codec: ZlinkStreamCodec.Protobuf,
        payload: payload.data()
      });
    }
  };
}

function inferMessageType(value: unknown): Function | undefined {
  if (value === null || value === undefined) return undefined;
  const constructor = (value as { constructor?: Function }).constructor;
  return constructor === Object ? undefined : constructor;
}
