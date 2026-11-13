import { Buffer } from 'node:buffer';
import * as profile from '@zlink-systems/stream-wire/internal/json';

export const FRAMEWORK_JSON_V1_PROFILE = profile.FRAMEWORK_JSON_V1_PROFILE;
export const stringifyFrameworkJsonV1 = profile.createFrameworkJsonV1Stringifier(encodeNodeBase64);

function encodeNodeBase64(bytes: Uint8Array): string {
  // Borrow the exact byte range. Native Base64 avoids a spread argument list
  // and an intermediate binary string without copying the input storage.
  return Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength).toString('base64');
}

const DEFAULT_PARSE_OPTIONS = Object.freeze({});

export function parseFrameworkJsonV1(
  text: string,
  options: profile.FrameworkJsonParseOptions = DEFAULT_PARSE_OPTIONS,
  schema?: Parameters<typeof profile.parseFrameworkJsonV1>[2],
  reviver?: Parameters<typeof profile.parseFrameworkJsonV1>[3]
): unknown {
  return profile.parseFrameworkJsonV1(text, options, schema, reviver, decodeNodeBase64);
}

function decodeNodeBase64(value: string): Uint8Array {
  // Buffer.from(string) may slice the shared Buffer pool; the decoded value must
  // own exactly its bytes (payload ownership §2/§5), so write into unpooled storage.
  const bytes = Buffer.allocUnsafeSlow(Buffer.byteLength(value, 'base64'));
  const written = bytes.write(value, 'base64');
  return new Uint8Array(bytes.buffer, 0, written);
}
