import {
  defaultMaxDecompressedPayloadSize,
  lz4PickleUncompressed as pickleUncompressed,
  lz4UnpicklePayload as unpicklePayload
} from './lz4-pickle';

export interface ZLinkStreamWireFrame {
  readonly header: Uint8Array;
  readonly payload: Uint8Array;
}

/** Numeric payload codec values shared by browser and server wire paths. */
export enum ZlinkStreamCodec {
  Raw = 0,
  Json = 1,
  MessagePack = 2,
  Protobuf = 3
}

export interface ZLinkStreamWireHeader {
  readonly kind: number;
  readonly codec: number;
  readonly flags: number;
  readonly requestSeq?: bigint;
  readonly name: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly correlationId?: string;
  readonly flowId?: string;
  readonly flowOrigin?: number;
}

export interface ZLinkStreamWireHeaderFlags {
  readonly hasRequestSeq: number;
  readonly hasMetadata: number;
  readonly hasCorrelationId: number;
  readonly hasFlowId: number;
}

const defaultHeaderFlags: ZLinkStreamWireHeaderFlags = {
  hasRequestSeq: 0x01,
  hasMetadata: 0x02,
  hasCorrelationId: 0x08,
  hasFlowId: 0x10
};

export const ZLINK_STREAM_FORMAT_MARKER = 0xf2;
const ZLINK_STREAM_RESPONSE_KIND = 3;
const ZLINK_STREAM_ERROR_KIND = 4;

export function encodeStreamWireFrame(header: Uint8Array, payload: Uint8Array): Uint8Array {
  if (header.length > 0xffff) {
    throw new Error('Stream header is too large.');
  }
  if (payload.length > 0xffffffff) {
    throw new Error('Stream payload is too large.');
  }
  const frame = new Uint8Array(6 + header.length + payload.length);
  writeUInt16BE(frame, 0, header.length);
  writeUInt32BE(frame, 2, payload.length);
  frame.set(header, 6);
  frame.set(payload, 6 + header.length);
  return frame;
}

export function decodeStreamWireFrame(frame: Uint8Array): ZLinkStreamWireFrame {
  if (frame.length < 6) {
    throw new Error('Stream frame prefix is incomplete.');
  }
  const headerLength = readUInt16BE(frame, 0);
  const payloadLength = readUInt32BE(frame, 2);
  if (frame.length !== 6 + headerLength + payloadLength) {
    throw new Error('Stream frame length does not match prefix.');
  }
  return {
    header: frame.slice(6, 6 + headerLength),
    payload: frame.slice(6 + headerLength)
  };
}

export function tryDecodeStreamWireFrame(frame: Uint8Array): ZLinkStreamWireFrame | undefined {
  if (frame.length < 6) {
    return undefined;
  }
  const headerLength = readUInt16BE(frame, 0);
  const payloadLength = readUInt32BE(frame, 2);
  if (frame.length !== 6 + headerLength + payloadLength) {
    return undefined;
  }
  return {
    header: frame.slice(6, 6 + headerLength),
    payload: frame.slice(6 + headerLength)
  };
}

export function encodeStreamWireHeader(
  header: ZLinkStreamWireHeader,
  flags = defaultHeaderFlags
): Uint8Array {
  const reply = isReplyKind(header.kind);
  const packetName = reply ? '' : header.name;
  if (!reply) validateStreamWirePacketName(packetName);
  const nameBytes = utf8Encode(packetName);
  const hasRequestSeq = header.requestSeq !== undefined;
  const hasMetadata = header.metadata.size > 0;
  const correlationBytes = header.correlationId !== undefined && header.correlationId.length > 0
    ? utf8Encode(header.correlationId)
    : undefined;
  if (correlationBytes !== undefined && correlationBytes.length > 0xff) {
    throw new Error('Stream correlation id is too large.');
  }
  const hasCorrelation = correlationBytes !== undefined;
  const hasFlow = header.flowId !== undefined || header.flowOrigin !== undefined;
  if (hasFlow && (header.flowId === undefined || header.flowOrigin === undefined)) {
    throw new Error('Stream flow id and origin must be provided together.');
  }
  if (header.flowId !== undefined) validateFlowId(header.flowId);
  if (header.flowOrigin !== undefined && ![1, 2, 3, 4].includes(header.flowOrigin)) {
    throw new Error('Stream flow origin is invalid.');
  }
  let headerFlags = header.flags;
  headerFlags = hasRequestSeq ? headerFlags | flags.hasRequestSeq : headerFlags & ~flags.hasRequestSeq;
  headerFlags = hasMetadata ? headerFlags | flags.hasMetadata : headerFlags & ~flags.hasMetadata;
  headerFlags = hasCorrelation ? headerFlags | flags.hasCorrelationId : headerFlags & ~flags.hasCorrelationId;
  headerFlags = hasFlow ? headerFlags | flags.hasFlowId : headerFlags & ~flags.hasFlowId;

  const metadataBytes = hasMetadata ? encodeStreamWireMetadata(header.metadata) : new Uint8Array();
  const size = 4 + (hasRequestSeq ? 8 : 0) + 1 + nameBytes.length
    + (hasMetadata ? 2 + metadataBytes.length : 0)
    + (hasCorrelation ? 1 + correlationBytes.length : 0)
    + (hasFlow ? 37 : 0);
  const buffer = new Uint8Array(size);
  let offset = 0;
  buffer[offset++] = ZLINK_STREAM_FORMAT_MARKER;
  buffer[offset++] = header.kind;
  buffer[offset++] = header.codec;
  buffer[offset++] = headerFlags;
  if (hasRequestSeq) {
    if (header.requestSeq === 0n) {
      throw new Error('Request sequence must not be zero.');
    }
    writeBigUInt64BE(buffer, offset, header.requestSeq);
    offset += 8;
  }
  buffer[offset++] = nameBytes.length;
  buffer.set(nameBytes, offset);
  offset += nameBytes.length;
  if (hasMetadata) {
    writeUInt16BE(buffer, offset, metadataBytes.length);
    offset += 2;
    buffer.set(metadataBytes, offset);
    offset += metadataBytes.length;
  }
  if (hasCorrelation) {
    buffer[offset++] = correlationBytes.length;
    buffer.set(correlationBytes, offset);
    offset += correlationBytes.length;
  }
  if (hasFlow) {
    buffer.set(asciiEncode(header.flowId!), offset);
    offset += 36;
    buffer[offset++] = header.flowOrigin!;
  }
  return buffer;
}

export function decodeStreamWireHeader(
  header: Uint8Array,
  flags = defaultHeaderFlags
): ZLinkStreamWireHeader {
  let offset = 0;
  if (header.length < 5) {
    throw new Error('Stream header is incomplete.');
  }
  if (header[offset++] !== ZLINK_STREAM_FORMAT_MARKER) {
    throw new Error('Stream header format marker is invalid.');
  }
  const kind = header[offset++];
  const codec = header[offset++];
  const headerFlags = header[offset++];
  const hasRequestSeq = (headerFlags & flags.hasRequestSeq) !== 0;
  const hasMetadata = (headerFlags & flags.hasMetadata) !== 0;
  const hasCorrelation = (headerFlags & flags.hasCorrelationId) !== 0;
  const hasFlow = (headerFlags & flags.hasFlowId) !== 0;
  if ((headerFlags & ~0x1f) !== 0) {
    throw new Error('Unknown mandatory stream header flag.');
  }
  let requestSeq: bigint | undefined;
  if (hasRequestSeq) {
    if (header.length - offset < 8) {
      throw new Error('Stream request sequence is incomplete.');
    }
    requestSeq = readBigUInt64BE(header, offset);
    if (requestSeq === 0n) {
      throw new Error('Request sequence must not be zero.');
    }
    offset += 8;
  }
  if (header.length - offset < 1) {
    throw new Error('Stream packet name length is missing.');
  }
  const nameLength = header[offset++];
  if ((!isReplyKind(kind) && nameLength === 0) || header.length - offset < nameLength) {
    throw new Error('Stream packet name is invalid.');
  }
  const decodedName = utf8Decode(header.subarray(offset, offset + nameLength));
  const name = isReplyKind(kind) ? '' : decodedName;
  offset += nameLength;
  const decodedMetadata = hasMetadata
    ? decodeStreamWireHeaderMetadata(header, offset)
    : { metadata: new Map<string, string>(), offset };
  offset = decodedMetadata.offset;
  let correlationId: string | undefined;
  if (hasCorrelation) {
    if (header.length - offset < 1) {
      throw new Error('Stream correlation id is incomplete.');
    }
    const correlationLength = header[offset++];
    if (header.length - offset < correlationLength) {
      throw new Error('Stream correlation id is incomplete.');
    }
    correlationId = utf8Decode(header.subarray(offset, offset + correlationLength));
    offset += correlationLength;
  }
  let flowId: string | undefined;
  let flowOrigin: number | undefined;
  if (hasFlow) {
    if (header.length - offset < 37) {
      throw new Error('Stream flow fields are incomplete.');
    }
    flowId = asciiDecode(header.subarray(offset, offset + 36));
    validateFlowId(flowId);
    offset += 36;
    flowOrigin = header[offset++];
    if (![1, 2, 3, 4].includes(flowOrigin)) {
      throw new Error('Stream flow origin is invalid.');
    }
  }
  if (offset !== header.length) {
    throw new Error('Stream header has trailing bytes.');
  }
  return {
    kind,
    codec,
    flags: headerFlags,
    requestSeq,
    name,
    metadata: decodedMetadata.metadata,
    correlationId,
    flowId,
    flowOrigin
  };
}

function validateFlowId(flowId: string): void {
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(flowId)) {
    throw new Error('Stream flow id must be a lowercase UUIDv7.');
  }
}

function asciiEncode(value: string): Uint8Array {
  return Uint8Array.from(value, (character) => character.charCodeAt(0));
}

function asciiDecode(value: Uint8Array): string {
  if (value.some((byte) => byte > 0x7f)) throw new Error('Stream flow id must be ASCII.');
  return String.fromCharCode(...value);
}

export function encodeStreamWireMetadata(metadata: ReadonlyMap<string, string>): Uint8Array {
  if (metadata.size > 255) {
    throw new Error('Metadata entry count must not exceed 255.');
  }
  let size = 1;
  const encoded = [...metadata].map(([key, value]) => {
    const keyBytes = utf8Encode(key);
    const valueBytes = utf8Encode(value);
    if (keyBytes.length === 0 || keyBytes.length > 255) {
      throw new Error('Metadata key length is invalid.');
    }
    if (valueBytes.length > 0xffff) {
      throw new Error('Metadata value is too large.');
    }
    size += 1 + keyBytes.length + 2 + valueBytes.length;
    return { keyBytes, valueBytes };
  });
  const buffer = new Uint8Array(size);
  let offset = 0;
  buffer[offset++] = metadata.size;
  for (const { keyBytes, valueBytes } of encoded) {
    buffer[offset++] = keyBytes.length;
    buffer.set(keyBytes, offset);
    offset += keyBytes.length;
    writeUInt16BE(buffer, offset, valueBytes.length);
    offset += 2;
    buffer.set(valueBytes, offset);
    offset += valueBytes.length;
  }
  return buffer;
}

export function decodeStreamWireMetadata(metadata: Uint8Array): Map<string, string> {
  const decoded = decodeStreamWireMetadataAt(metadata, 0, metadata.length);
  if (decoded.offset !== metadata.length) {
    throw new Error('Stream metadata payload has trailing bytes.');
  }
  return decoded.metadata;
}

export function lz4PickleUncompressed(payload: Uint8Array): Uint8Array {
  return pickleUncompressed(payload);
}

export function lz4UnpicklePayload(
  payload: Uint8Array,
  maxDecompressedSize = defaultMaxDecompressedPayloadSize
): Uint8Array {
  return unpicklePayload(payload, maxDecompressedSize);
}

export function utf8Encode(value: string): Uint8Array {
  return new TextEncoder().encode(value);
}

export function utf8Decode(value: Uint8Array): string {
  return new TextDecoder().decode(value);
}

function decodeStreamWireHeaderMetadata(
  header: Uint8Array,
  offset: number
): { metadata: Map<string, string>; offset: number } {
  if (header.length - offset < 2) {
    throw new Error('Stream metadata section is incomplete.');
  }
  const metadataLength = readUInt16BE(header, offset);
  offset += 2;
  if (header.length - offset < metadataLength) {
    throw new Error('Stream metadata payload is incomplete.');
  }
  return decodeStreamWireMetadataAt(header, offset, offset + metadataLength);
}

function decodeStreamWireMetadataAt(
  source: Uint8Array,
  offset: number,
  end: number
): { metadata: Map<string, string>; offset: number } {
  if (offset >= end) {
    throw new Error('Stream metadata entry count is missing.');
  }
  const count = source[offset++];
  const metadata = new Map<string, string>();
  for (let index = 0; index < count; index += 1) {
    if (offset >= end) {
      throw new Error('Stream metadata key length is missing.');
    }
    const keyLength = source[offset++];
    if (keyLength === 0 || end - offset < keyLength) {
      throw new Error('Stream metadata key is invalid.');
    }
    const key = utf8Decode(source.subarray(offset, offset + keyLength));
    offset += keyLength;
    if (end - offset < 2) {
      throw new Error('Stream metadata value length is missing.');
    }
    const valueLength = readUInt16BE(source, offset);
    offset += 2;
    if (end - offset < valueLength) {
      throw new Error('Stream metadata value is incomplete.');
    }
    if (metadata.has(key)) {
      throw new Error('Duplicate metadata key is duplicated.');
    }
    metadata.set(key, utf8Decode(source.subarray(offset, offset + valueLength)));
    offset += valueLength;
  }
  if (offset !== end) {
    throw new Error('Stream metadata payload has trailing bytes.');
  }
  return { metadata, offset };
}

function validateStreamWirePacketName(name: string): void {
  const nameBytes = utf8Encode(name);
  if (name.trim().length === 0 || nameBytes.length > 255) {
    throw new Error('Stream packet name is invalid.');
  }
}

function isReplyKind(kind: number): boolean {
  return kind === ZLINK_STREAM_RESPONSE_KIND || kind === ZLINK_STREAM_ERROR_KIND;
}

function writeUInt16BE(buffer: Uint8Array, offset: number, value: number): void {
  buffer[offset] = (value >>> 8) & 0xff;
  buffer[offset + 1] = value & 0xff;
}

function readUInt16BE(buffer: Uint8Array, offset: number): number {
  return (buffer[offset] << 8) | buffer[offset + 1];
}

function readUInt32BE(buffer: Uint8Array, offset: number): number {
  return (
    buffer[offset] * 0x1000000
    + ((buffer[offset + 1] << 16) | (buffer[offset + 2] << 8) | buffer[offset + 3])
  );
}

function writeUInt32BE(buffer: Uint8Array, offset: number, value: number): void {
  buffer[offset] = (value >>> 24) & 0xff;
  buffer[offset + 1] = (value >>> 16) & 0xff;
  buffer[offset + 2] = (value >>> 8) & 0xff;
  buffer[offset + 3] = value & 0xff;
}

function writeBigUInt64BE(buffer: Uint8Array, offset: number, value: bigint): void {
  for (let index = 7; index >= 0; index -= 1) {
    buffer[offset + index] = Number(value & 0xffn);
    value >>= 8n;
  }
}

function readBigUInt64BE(buffer: Uint8Array, offset: number): bigint {
  let value = 0n;
  for (let index = 0; index < 8; index += 1) {
    value = (value << 8n) | BigInt(buffer[offset + index]);
  }
  return value;
}
