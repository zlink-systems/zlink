import {
  ZLinkEncodedPayload,
  type ZLinkCodecExtension,
  type ZLinkCodecRegistrar,
  type ZLinkMessageSerializer
} from '@zlink-systems/framework';
import type { ZlinkStreamPayloadCodec } from '@zlink-systems/stream-connector';
import {
  decodeMessagePack,
  encodeMessagePack,
  ZLINK_MESSAGEPACK_CONTENT_TYPE,
  zlinkStreamMessagePackCodec
} from './index';

export { ZLINK_MESSAGEPACK_CONTENT_TYPE } from './index';

export type ZLinkMessagePackCodecExtension = ZLinkCodecExtension & ZlinkStreamPayloadCodec;

export function zlinkMessagePackCodec(): ZLinkMessagePackCodecExtension {
  return {
    register(codecs: ZLinkCodecRegistrar): void {
      codecs.addSerializer(ZLINK_MESSAGEPACK_CONTENT_TYPE, createMessagePackSerializer());
      codecs.addStreamCodec(ZLINK_MESSAGEPACK_CONTENT_TYPE, this);
    },
    encode: zlinkStreamMessagePackCodec.encode,
    decode: zlinkStreamMessagePackCodec.decode
  };
}

export function createMessagePackSerializer(): ZLinkMessageSerializer {
  return {
    serialize<T>(value: T): ZLinkEncodedPayload {
      return ZLinkEncodedPayload.from(encodeMessagePack(value));
    },
    deserialize<T>(payload: ZLinkEncodedPayload): T {
      return decodeMessagePack(payload.data()) as T;
    }
  };
}
