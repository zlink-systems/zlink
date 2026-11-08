export const MAX_TIME_MS = 9_223_372_036_854n;
const MAX_PAYLOAD_BYTES = 1_048_576;
const MAX_REVISION = 9_223_372_036_854_775_807n;

export class StoreFormatError extends Error {
  constructor() {
    super('Invalid etcd Store encoding');
  }
}

export function validateNamespace(namespace: string): void {
  if (typeof namespace !== 'string' || namespace.includes('\0'))
    throw new TypeError('Invalid namespace');
  const length = Buffer.byteLength(namespace, 'utf8');
  if (length < 1 || length > 64) throw new RangeError('Namespace must contain 1..64 UTF-8 bytes');
}

export function keys(namespace: string) {
  validateNamespace(namespace);
  const namespacePrefix = Buffer.from(`${namespace}\0`);
  const metadataPrefix = Buffer.concat([namespacePrefix, Buffer.from('m\0')]);
  const payloadPrefix = Buffer.concat([namespacePrefix, Buffer.from('p\0')]);
  return {
    namespacePrefix,
    metadataPrefix,
    payloadPrefix,
    clock: Buffer.concat([namespacePrefix, Buffer.from('c')]),
    metadata: (key: Uint8Array) => Buffer.concat([metadataPrefix, key]),
    payload: (key: Uint8Array) => Buffer.concat([payloadPrefix, key])
  };
}

export function prefixEnd(prefix: Buffer): Buffer {
  const end = Buffer.from(prefix);
  for (let i = end.length - 1; i >= 0; i--) {
    if (end[i] < 255) {
      end[i]++;
      return end.subarray(0, i + 1);
    }
  }
  return Buffer.from([0]);
}

export function encodeClock(value: bigint): Buffer {
  if (value < 0n || value > MAX_TIME_MS)
    throw new RangeError('Store time exceeds its encoding range');
  const bytes = Buffer.alloc(8);
  bytes.writeBigUInt64BE(value);
  return bytes;
}

export function decodeClock(bytes: Buffer): bigint {
  if (bytes.length !== 8) throw new StoreFormatError();
  const value = bytes.readBigUInt64BE();
  if (value > MAX_TIME_MS) throw new StoreFormatError();
  return value;
}

export interface Metadata {
  hasExpiry: boolean;
  expiresAtMs: bigint;
  payloadLength: number;
}

export function encodeMetadata(value: Metadata): Buffer {
  if (
    !Number.isInteger(value.payloadLength) ||
    value.payloadLength < 0 ||
    value.payloadLength > MAX_PAYLOAD_BYTES ||
    (!value.hasExpiry && value.expiresAtMs !== 0n)
  )
    throw new RangeError('Invalid metadata');
  const bytes = Buffer.alloc(14);
  bytes[0] = 1;
  bytes[1] = value.hasExpiry ? 1 : 0;
  encodeClock(value.expiresAtMs).copy(bytes, 2);
  bytes.writeUInt32BE(value.payloadLength, 10);
  return bytes;
}

export function decodeMetadata(bytes: Buffer): Metadata {
  if (bytes.length !== 14 || bytes[0] !== 1 || bytes[1] > 1) throw new StoreFormatError();
  const value = {
    hasExpiry: bytes[1] === 1,
    expiresAtMs: decodeClock(bytes.subarray(2, 10)),
    payloadLength: bytes.readUInt32BE(10)
  };
  if ((!value.hasExpiry && value.expiresAtMs !== 0n) || value.payloadLength > MAX_PAYLOAD_BYTES)
    throw new StoreFormatError();
  return value;
}

export function validateRecord(
  metadata: Buffer | null,
  payload: Buffer | null,
  metadataRevision: string,
  payloadRevision: string
): Metadata | null {
  if (metadata === null && payload === null) return null;
  if (metadata === null || payload === null || metadataRevision !== payloadRevision)
    throw new StoreFormatError();
  const value = decodeMetadata(metadata);
  if (value.payloadLength !== payload.length) throw new StoreFormatError();
  return value;
}

export function encodeVersion(clusterId: string, revision: string): string {
  const cluster = BigInt(clusterId);
  if (
    cluster < 0n ||
    cluster > 0xffffffffffffffffn ||
    !/^[1-9][0-9]*$/.test(revision) ||
    BigInt(revision).toString() !== revision ||
    BigInt(revision) > MAX_REVISION
  )
    throw new RangeError('Invalid version');
  return `e1:${cluster.toString(16).padStart(16, '0')}:${revision}`;
}

export function decodeVersion(value: string): { clusterId: string; revision: string } | null {
  const match = /^e1:([0-9a-f]{16}):([1-9][0-9]*)$/.exec(value);
  if (match === null || match[0] !== value || BigInt(match[2]) > MAX_REVISION) return null;
  return { clusterId: BigInt(`0x${match[1]}`).toString(), revision: match[2] };
}
