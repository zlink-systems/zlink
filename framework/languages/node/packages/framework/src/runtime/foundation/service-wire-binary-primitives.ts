import type { RoutingId } from '../../contracts';
import {
  decodeRoutingId,
  encodeRoutingIdStorageHex
} from '../routing-id';

export type ServiceWireInvalid = (message: string) => never;

/** Encodes text without silently replacing an unpaired UTF-16 surrogate. */
export function encodeCanonicalServiceWireText(
  value: string,
  field: string,
  maximumBytes: number,
  invalid: ServiceWireInvalid
): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (
    bytes.byteLength < 1
    || bytes.byteLength > maximumBytes
    || value.includes('\0')
    || bytes.toString('utf8') !== value
  ) {
    invalid(`${field} must be 1..${maximumBytes} canonical UTF-8 bytes without NUL.`);
  }
  return bytes;
}

/** Decodes schema text and rejects Node's replacement-character fallback. */
export function decodeCanonicalServiceWireText(
  bytes: Uint8Array,
  field: string,
  invalid: ServiceWireInvalid
): string {
  const encoded = Buffer.from(bytes);
  const value = encoded.toString('utf8');
  if (
    value.includes('\0')
    || !Buffer.from(value, 'utf8').equals(encoded)
  ) {
    invalid(`${field} is not canonical UTF-8 without NUL.`);
  }
  return value;
}

/** Preserves the schema's binary-safe RoutingId bytes. */
export function encodeServiceWireRoutingId(
  value: RoutingId,
  field: string,
  maximumBytes: number,
  invalid: ServiceWireInvalid
): Buffer {
  let bytes: Buffer;
  try {
    bytes = Buffer.from(encodeRoutingIdStorageHex(value), 'hex');
  } catch (error) {
    invalid(`${field} is not a valid RoutingId: ${errorMessage(error)}`);
  }
  if (bytes.byteLength < 1 || bytes.byteLength > maximumBytes) {
    invalid(`${field} must be 1..${maximumBytes} opaque bytes.`);
  }
  return bytes;
}

/** Returns printable UTF-8 as a string and otherwise retains an opaque byte identity. */
export function decodeServiceWireRoutingId(
  bytes: Uint8Array,
  field: string,
  maximumBytes: number,
  invalid: ServiceWireInvalid
): RoutingId {
  const encoded = Buffer.from(bytes);
  if (encoded.byteLength < 1 || encoded.byteLength > maximumBytes) {
    invalid(`${field} must be 1..${maximumBytes} opaque bytes.`);
  }
  return decodeRoutingId(encoded.toString('utf8'), encoded.toString('hex'));
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
