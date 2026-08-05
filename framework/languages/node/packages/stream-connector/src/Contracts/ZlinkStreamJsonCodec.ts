import {
  ZlinkStreamCodec
} from './ZlinkStreamEnums';
import type {
  ZlinkStreamPayloadCodec
} from './ZlinkStreamConnectorOptions';
import type {
  ZlinkStreamEncodedPayload
} from './ZlinkStreamModels';

export const zlinkStreamJsonCodecName = 'json';

export interface ZlinkStreamJsonCodecOptions {
  readonly replacer?: (this: unknown, key: string, value: unknown) => unknown;
  readonly reviver?: (this: unknown, key: string, value: unknown) => unknown;
}

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

  decode<T = unknown>(payload: ZlinkStreamEncodedPayload): T {
    return fromJson<T>(payload);
  }
};

export function toJson<T>(value: T, messageType?: Function): ZlinkStreamEncodedPayload {
  return {
    codec: ZlinkStreamCodec.Json,
    payload: new TextEncoder().encode(JSON.stringify(value, codecOptions.replacer)),
    messageType: messageType ?? inferMessageType(value)
  };
}

export function fromJson<T>(payload: ZlinkStreamEncodedPayload): T {
  ensureJson(payload);
  return JSON.parse(new TextDecoder().decode(payload.payload), safeJsonReviver) as T;
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

function safeJsonReviver(this: unknown, key: string, value: unknown): unknown {
  if (isPrototypeKey(key)) {
    throw new Error(`JSON payload key '${key}' is not allowed.`);
  }
  if (codecOptions.reviver !== undefined) {
    return codecOptions.reviver.call(this, key, value);
  }
  return value;
}

function isPrototypeKey(key: string): boolean {
  return key === '__proto__' || key === 'constructor' || key === 'prototype';
}
