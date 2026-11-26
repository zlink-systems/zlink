import { SERVICE_WIRE_METADATA_BYTES as MAX_METADATA_BYTES } from './service-wire-constants.generated';
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });

export function encodeServiceMetadataFrame(entries: ReadonlyMap<string, string>): Buffer {
  if (entries.size > 0xff) {
    throw new RangeError('Application metadata contains more than 255 entries.');
  }
  if (metadataExceedsLimit(Array.from(entries, ([key, value]) => ({ key, value })))) {
    throw new RangeError(`Application metadata exceeds ${MAX_METADATA_BYTES} bytes.`);
  }
  const encoded: Buffer[] = [Buffer.of(1, entries.size)];
  for (const [key, value] of entries) {
    const keyBytes = textBytes(key, 1, 0xff, 'metadata key');
    const valueBytes = textBytes(value, 0, 0xffff, 'metadata value');
    const valueLength = Buffer.allocUnsafe(2);
    valueLength.writeUInt16BE(valueBytes.byteLength);
    encoded.push(Buffer.of(keyBytes.byteLength), keyBytes, valueLength, valueBytes);
  }
  return Buffer.concat(encoded);
}

export function validateServiceMetadataFrame(frame: Uint8Array): Buffer {
  if (frame.byteLength > MAX_METADATA_BYTES) {
    throw new RangeError(`Application metadata frame exceeds ${MAX_METADATA_BYTES} bytes.`);
  }
  const bytes = Buffer.from(frame);
  let offset = 0;
  const take = (length: number): Buffer => {
    if (length < 0 || offset + length > bytes.byteLength) {
      throw new TypeError('Application metadata frame is truncated.');
    }
    const value = bytes.subarray(offset, offset + length);
    offset += length;
    return value;
  };
  if (take(1)[0] !== 1) {
    throw new TypeError('Application metadata frame version is invalid.');
  }
  const count = take(1)[0]!;
  const keys = new Set<string>();
  const entries: { key: string; value: string }[] = [];
  for (let index = 0; index < count; index += 1) {
    const key = take(take(1)[0]!);
    if (key.byteLength === 0 || key.includes(0)) {
      throw new TypeError('Application metadata key is invalid.');
    }
    const decodedKey = FATAL_UTF8.decode(key);
    const keyIdentity = key.toString('hex');
    if (keys.has(keyIdentity)) {
      throw new TypeError('Application metadata keys must be unique.');
    }
    keys.add(keyIdentity);
    const valueLength = take(2).readUInt16BE();
    const value = take(valueLength);
    if (value.includes(0)) {
      throw new TypeError('Application metadata value is invalid.');
    }
    entries.push({ key: decodedKey, value: FATAL_UTF8.decode(value) });
  }
  if (offset !== bytes.byteLength) {
    throw new TypeError('Application metadata frame has trailing bytes.');
  }
  if (metadataExceedsLimit(entries)) {
    throw new RangeError(`Application metadata exceeds ${MAX_METADATA_BYTES} bytes.`);
  }
  return bytes;
}

function textBytes(value: string, minimum: number, maximum: number, name: string): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < minimum || bytes.byteLength > maximum || bytes.includes(0)) {
    throw new RangeError(`${name} must contain ${minimum}..${maximum} UTF-8 bytes without NUL.`);
  }
  return bytes;
}

// Application metadata limit (06-wire-protocol §2 "Decode 검증과 크기 상한"): the sum of every
// entry encoded as ChannelHeader field 13 must not exceed the limit, for every command and both
// directions. Callers raise their own error type; this is the only comparison with the limit.
export function metadataExceedsLimit(entries: readonly { key: string; value: string }[]): boolean {
  return protobufMetadataSize(entries) > MAX_METADATA_BYTES;
}

function protobufMetadataSize(entries: readonly { key: string; value: string }[]): number {
  // Count field 13 and each nested string field with their wire tags and length prefixes.
  // proto3 omits an empty value field.
  let size = 0;
  for (const entry of entries) {
    const keyBytes = Buffer.byteLength(entry.key, 'utf8');
    const valueBytes = Buffer.byteLength(entry.value, 'utf8');
    const bodyBytes =
      1 +
      protobufVarintSize(keyBytes) +
      keyBytes +
      (valueBytes === 0 ? 0 : 1 + protobufVarintSize(valueBytes) + valueBytes);
    size += 1 + protobufVarintSize(bodyBytes) + bodyBytes;
  }
  return size;
}

function protobufVarintSize(value: number): number {
  let size = 1;
  while (value >= 0x80) {
    value = Math.floor(value / 0x80);
    size++;
  }
  return size;
}
