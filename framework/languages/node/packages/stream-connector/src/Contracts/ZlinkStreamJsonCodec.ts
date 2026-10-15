import {
  stringifyFrameworkJsonV1,
  parseFrameworkJsonV1,
  readFrameworkPacketJsonContract,
  isPrototypeKey
} from '@zlink-systems/stream-wire/internal/json';
import { ZlinkStreamCodec } from './ZlinkStreamEnums';
import type { ZlinkStreamPayloadCodec } from './ZlinkStreamConnectorOptions';
import type { ZlinkStreamEncodedPayload } from './ZlinkStreamModels';

export const zlinkStreamJsonCodecName = 'json';

export interface ZlinkStreamJsonCodecOptions {
  readonly replacer?: (this: unknown, key: string, value: unknown) => unknown;
  readonly reviver?: (this: unknown, key: string, value: unknown) => unknown;
}

const encoder = new TextEncoder();
const decoder = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

let codecOptions: ZlinkStreamJsonCodecOptions = {};

export const zlinkStreamJsonCodec: ZlinkStreamPayloadCodec & {
  configure(options: ZlinkStreamJsonCodecOptions): void;
} = {
  configure(options: ZlinkStreamJsonCodecOptions): void {
    codecOptions = options;
  },

  encode(payload: unknown, messageType?: Function): ZlinkStreamEncodedPayload {
    return toJson(payload, messageType);
  },

  decode<T = unknown>(payload: ZlinkStreamEncodedPayload, messageType?: Function): T {
    return decodeJson<T>(payload, messageType ?? payload.messageType);
  }
};

export function toJson<T>(value: T, messageType?: Function): ZlinkStreamEncodedPayload {
  const type = messageType ?? inferMessageType(value);
  return {
    codec: ZlinkStreamCodec.Json,
    payload: encoder.encode(
      stringifyFrameworkJsonV1(
        value,
        type === undefined ? undefined : readFrameworkPacketJsonContract(type)?.payload,
        codecOptions.replacer
      )
    ),
    messageType: type
  };
}

export function fromJson<T>(payload: ZlinkStreamEncodedPayload): T {
  return decodeJson<T>(payload, payload.messageType);
}

function decodeJson<T>(payload: ZlinkStreamEncodedPayload, messageType?: Function): T {
  ensureJson(payload);
  return parseFrameworkJsonV1(
    decoder.decode(payload.payload),
    { rejectPropertyName: isPrototypeKey },
    messageType === undefined ? undefined : readFrameworkPacketJsonContract(messageType)?.payload,
    codecOptions.reviver
  ) as T;
}

function ensureJson(payload: ZlinkStreamEncodedPayload): void {
  if (payload.codec !== ZlinkStreamCodec.Json) {
    throw new Error(`Stream payload codec is ${payload.codec}, not Json.`);
  }
}

function inferMessageType(value: unknown): Function | undefined {
  if (value === null || value === undefined) {
    return undefined;
  }
  const constructor = Object.getPrototypeOf(value)?.constructor;
  return constructor === Object ? undefined : constructor;
}
