import type { RoutingId } from '../../contracts';
import { decodeRoutingId, encodeRoutingIdStorageHex } from '../routing-id';
export const SERVICE_WIRE_MAJOR_OFFSET = 2;
export const SERVICE_WIRE_COMMAND_OFFSET = SERVICE_WIRE_MAJOR_OFFSET + 1;
export const SERVICE_WIRE_FLAGS_OFFSET = SERVICE_WIRE_COMMAND_OFFSET + 1;
export const SERVICE_WIRE_PREFIX_SIZE = SERVICE_WIRE_FLAGS_OFFSET + 1;

export const SERVICE_WIRE_TEXT8_MAX_BYTES = 255;

// Fatal decoding accepts exactly the canonical UTF-8 encodings; a leading
// U+FEFF stays part of the value.
const CANONICAL_UTF8 = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true });

export type ServiceWireInvalid = (message: string) => never;

/** String.prototype.isWellFormed (ES2024) exists on every supported Node (20 and later). */
type ServiceWireText = string & { isWellFormed(): boolean };

/**
 * Validates text for the service wire and returns its UTF-8 byte length. An
 * unpaired UTF-16 surrogate is rejected instead of being replaced.
 */
export function canonicalServiceWireTextLength(
  value: string,
  field: string,
  maximumBytes: number,
  invalid: ServiceWireInvalid
): number {
  const length = Buffer.byteLength(value, 'utf8');
  if (
    length < 1 ||
    length > maximumBytes ||
    value.includes('\0') ||
    !(value as ServiceWireText).isWellFormed()
  ) {
    invalid(`${field} must be 1..${maximumBytes} canonical UTF-8 bytes without NUL.`);
  }
  return length;
}

/** Encodes text without silently replacing an unpaired UTF-16 surrogate. */
export function encodeCanonicalServiceWireText(
  value: string,
  field: string,
  maximumBytes: number,
  invalid: ServiceWireInvalid
): Buffer {
  canonicalServiceWireTextLength(value, field, maximumBytes, invalid);
  return Buffer.from(value, 'utf8');
}

/** Decodes schema text in place and rejects non-canonical UTF-8 and NUL. */
export function decodeCanonicalServiceWireText(
  bytes: Uint8Array,
  field: string,
  invalid: ServiceWireInvalid
): string {
  let value: string;
  try {
    value = CANONICAL_UTF8.decode(bytes);
  } catch {
    invalid(`${field} is not canonical UTF-8 without NUL.`);
  }
  if (value.includes('\0')) invalid(`${field} is not canonical UTF-8 without NUL.`);
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
