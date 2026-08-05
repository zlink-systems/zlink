const MAX_METADATA_BYTES = 1024;
const FATAL_UTF8 = new TextDecoder('utf-8', { fatal: true });

export function encodeServiceMetadataFrame(
  entries: ReadonlyMap<string, string>
): Buffer {
  if (entries.size > 0xff) {
    throw new RangeError('Application metadata contains more than 255 entries.');
  }
  const encoded: Buffer[] = [Buffer.of(1, entries.size)];
  for (const [key, value] of entries) {
    const keyBytes = textBytes(key, 1, 0xff, 'metadata key');
    const valueBytes = textBytes(value, 0, 0xffff, 'metadata value');
    const valueLength = Buffer.allocUnsafe(2);
    valueLength.writeUInt16BE(valueBytes.byteLength);
    encoded.push(Buffer.of(keyBytes.byteLength), keyBytes, valueLength, valueBytes);
  }
  return validateServiceMetadataFrame(Buffer.concat(encoded));
}

export function validateServiceMetadataFrame(frame: Uint8Array): Buffer {
  if (frame.byteLength > MAX_METADATA_BYTES) {
    throw new RangeError('Application metadata frame exceeds 1024 bytes.');
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
  for (let index = 0; index < count; index += 1) {
    const key = take(take(1)[0]!);
    if (key.byteLength === 0 || key.includes(0)) {
      throw new TypeError('Application metadata key is invalid.');
    }
    FATAL_UTF8.decode(key);
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
    FATAL_UTF8.decode(value);
  }
  if (offset !== bytes.byteLength) {
    throw new TypeError('Application metadata frame has trailing bytes.');
  }
  return Buffer.from(bytes);
}

function textBytes(
  value: string,
  minimum: number,
  maximum: number,
  name: string
): Buffer {
  const bytes = Buffer.from(value, 'utf8');
  if (bytes.byteLength < minimum || bytes.byteLength > maximum || bytes.includes(0)) {
    throw new RangeError(`${name} must contain ${minimum}..${maximum} UTF-8 bytes without NUL.`);
  }
  return bytes;
}
