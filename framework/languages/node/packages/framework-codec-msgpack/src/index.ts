import * as msgpack from '@msgpack/msgpack';
import {
  type ZlinkStreamEncodedPayload,
  type ZlinkStreamPayloadCodec
} from '@zlink-systems/stream-connector';
import { ZlinkStreamCodec } from '@zlink-systems/stream-wire';

export const ZLINK_MESSAGEPACK_CONTENT_TYPE = 'application/x-msgpack';
export const zlinkStreamMessagePackCodecName = 'messagepack';

export const zlinkStreamMessagePackCodec: ZlinkStreamPayloadCodec = {
  encode: toMsgPack,
  decode: fromMsgPack
};

export function toMsgPack<T>(value: T, messageType?: Function): ZlinkStreamEncodedPayload {
  return {
    codec: ZlinkStreamCodec.MessagePack,
    payload: encodeMessagePack(value),
    messageType: messageType ?? inferMessageType(value)
  };
}

export function fromMsgPack<T>(payload: ZlinkStreamEncodedPayload): T {
  ensureMessagePack(payload);
  return decodeMessagePack(payload.payload) as T;
}

export function encodeMessagePack(value: unknown): Uint8Array {
  return msgpack.encode(value);
}

export function decodeMessagePack(value: Uint8Array): unknown {
  return msgpack.decode(value);
}

function ensureMessagePack(payload: ZlinkStreamEncodedPayload): void {
  if (payload.codec !== ZlinkStreamCodec.MessagePack) {
    throw new Error(`Stream payload codec is ${payload.codec}, not MessagePack.`);
  }
}

function inferMessageType(value: unknown): Function | undefined {
  if (value === null || value === undefined) {
    return undefined;
  }
  const constructor = Object.getPrototypeOf(value)?.constructor;
  return constructor === Object ? undefined : constructor;
}
