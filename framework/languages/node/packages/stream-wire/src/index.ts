import {
  defaultMaxDecompressedPayloadSize,
  lz4PickledLength as pickledLength,
  lz4PickleUncompressed as pickleUncompressed,
  lz4UnpicklePayload as unpicklePayload
} from './lz4-pickle';

export { defaultMaxDecompressedPayloadSize } from './lz4-pickle';

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

export enum ZlinkStreamMessageKind {
  Send = 1,
  Request = 2,
  Response = 3,
  Error = 4,
  Control = 5
}

export enum ZlinkStreamHeaderFlags {
  None = 0,
  HasRequestSeq = 0x01,
  HasMetadata = 0x02,
  PayloadCompressed = 0x04,
  HasCorrelationId = 0x08,
  HasFlowId = 0x10,
  HasActorSlot = 0x20
}

export enum ZlinkStreamCloseReasonCode {
  ClientClose = 1,
  IdleTimeout = 2,
  HeartbeatTimeout = 3,
  ServerDrain = 4,
  ProtocolError = 5,
  TransportError = 6
}

export type ZlinkStreamCloseReason = keyof typeof ZlinkStreamCloseReasonCode;

const validMessageKinds = new Set(
  Object.values(ZlinkStreamMessageKind).filter((value) => typeof value === 'number')
);
const validCodecs = new Set(
  Object.values(ZlinkStreamCodec).filter((value) => typeof value === 'number')
);

export function isStreamWireMessageKind(value: number): boolean {
  return validMessageKinds.has(value);
}

export function isStreamWireCodec(value: number): boolean {
  return validCodecs.has(value);
}

const UTF8_TWO_BYTE_MIN = 0x80;
const UTF8_THREE_BYTE_MIN = 0x800;
const UTF16_HIGH_SURROGATE_MIN = 0xd800;
const UTF16_HIGH_SURROGATE_MAX = 0xdbff;
const UTF16_LOW_SURROGATE_MIN = 0xdc00;
const UTF16_LOW_SURROGATE_MAX = 0xdfff;
const UINT8_MAX = 0xff;
const UINT16_MAX = 0xffff;
const UINT16_BYTES = 2;
const UINT32_BYTES = 4;
export const UINT64_BYTES = 8;
export const UINT64_MAX = 0xffff_ffff_ffff_ffffn;
export const UINT32_MAX = 0xffff_ffff;
export const ZLINK_STREAM_FRAME_PREFIX_BYTES = UINT16_BYTES + UINT32_BYTES;
export const ZLINK_STREAM_MAX_PACKET_NAME_BYTES = UINT8_MAX;
export const ZLINK_STREAM_MAX_METADATA_BYTES = 1024;
const FLOW_ID_BYTES = 36;
const FLOW_FIELDS_BYTES = FLOW_ID_BYTES + 1;
const HEADER_MIN_BYTES = 5;
const HEADER_FLAG_MASK =
  ZlinkStreamHeaderFlags.HasRequestSeq |
  ZlinkStreamHeaderFlags.HasMetadata |
  ZlinkStreamHeaderFlags.PayloadCompressed |
  ZlinkStreamHeaderFlags.HasCorrelationId |
  ZlinkStreamHeaderFlags.HasFlowId |
  ZlinkStreamHeaderFlags.HasActorSlot;
const CONTROL_PAYLOAD_VERSION = 1;
const ACTOR_SLOT_OFFSET = 1;
const ACTOR_ID_LENGTH_OFFSET = ACTOR_SLOT_OFFSET + UINT16_BYTES;
const ACTOR_BOUND_PREFIX_BYTES = ACTOR_ID_LENGTH_OFFSET + 1;
const ACTOR_UNBOUND_BYTES = ACTOR_SLOT_OFFSET + UINT16_BYTES;
const CLOSING_DIAGNOSTIC_LENGTH_OFFSET = 2;
const CLOSING_PREFIX_BYTES = CLOSING_DIAGNOSTIC_LENGTH_OFFSET + UINT16_BYTES;
const MAX_CLOSING_DIAGNOSTIC_BYTES = 512;

export function encodeStreamWireActorBoundPayload(actorSlot: number, actorId: string): Uint8Array {
  const actorIdBytes = utf8Encode(actorId);
  if (actorIdBytes.length < 1 || actorIdBytes.length > UINT8_MAX) {
    throw new Error('Actor id length is invalid for a STREAM binding control packet.');
  }
  const payload = new Uint8Array(ACTOR_BOUND_PREFIX_BYTES + actorIdBytes.length);
  payload[0] = CONTROL_PAYLOAD_VERSION;
  writeUInt16BE(payload, ACTOR_SLOT_OFFSET, actorSlot);
  payload[ACTOR_ID_LENGTH_OFFSET] = actorIdBytes.length;
  payload.set(actorIdBytes, ACTOR_BOUND_PREFIX_BYTES);
  return payload;
}

export function decodeStreamWireActorBoundPayload(payload: Uint8Array): {
  readonly slot: number;
  readonly actorId: string;
} {
  if (payload.length < ACTOR_BOUND_PREFIX_BYTES + 1 || payload[0] !== CONTROL_PAYLOAD_VERSION) {
    throw new Error('Actor bound payload is invalid.');
  }
  const slot = readUInt16BE(payload, ACTOR_SLOT_OFFSET);
  const idLength = payload[ACTOR_ID_LENGTH_OFFSET];
  if (slot === 0 || idLength === 0 || payload.length !== ACTOR_BOUND_PREFIX_BYTES + idLength) {
    throw new Error('Actor bound payload is invalid.');
  }
  try {
    return { slot, actorId: decodeControlText(payload.subarray(ACTOR_BOUND_PREFIX_BYTES)) };
  } catch (cause) {
    throw new Error('Actor id is not valid UTF-8.', { cause });
  }
}

export function encodeStreamWireActorUnboundPayload(actorSlot: number): Uint8Array {
  const payload = new Uint8Array(ACTOR_UNBOUND_BYTES);
  payload[0] = CONTROL_PAYLOAD_VERSION;
  writeUInt16BE(payload, ACTOR_SLOT_OFFSET, actorSlot);
  return payload;
}

export function decodeStreamWireActorUnboundPayload(payload: Uint8Array): number {
  if (payload.length !== ACTOR_UNBOUND_BYTES || payload[0] !== CONTROL_PAYLOAD_VERSION) {
    throw new Error('Actor unbound payload is invalid.');
  }
  return readUInt16BE(payload, ACTOR_SLOT_OFFSET);
}

export function encodeStreamWireSessionClosingPayload(
  reasonCode: number,
  diagnostic = ''
): Uint8Array {
  const bytes = utf8Encode(diagnostic);
  if (bytes.length > MAX_CLOSING_DIAGNOSTIC_BYTES) {
    throw new Error('Session-closing diagnostic is too large.');
  }
  const payload = new Uint8Array(CLOSING_PREFIX_BYTES + bytes.length);
  payload[0] = CONTROL_PAYLOAD_VERSION;
  payload[1] = reasonCode;
  writeUInt16BE(payload, CLOSING_DIAGNOSTIC_LENGTH_OFFSET, bytes.length);
  payload.set(bytes, CLOSING_PREFIX_BYTES);
  return payload;
}

export function decodeStreamWireSessionClosing(payload: Uint8Array): {
  readonly closeReason: ZlinkStreamCloseReason;
  readonly diagnostic?: string;
} {
  if (payload.length < CLOSING_PREFIX_BYTES || payload[0] !== CONTROL_PAYLOAD_VERSION) {
    throw new Error('Unsupported session-closing version.');
  }
  const closeReason = ZlinkStreamCloseReasonCode[payload[1]] as ZlinkStreamCloseReason | undefined;
  if (closeReason === undefined) throw new Error('Unknown session-closing reason.');
  const length = readUInt16BE(payload, CLOSING_DIAGNOSTIC_LENGTH_OFFSET);
  if (length > MAX_CLOSING_DIAGNOSTIC_BYTES || payload.length !== CLOSING_PREFIX_BYTES + length) {
    throw new Error('Invalid session-closing diagnostic length.');
  }
  const diagnostic =
    length === 0 ? undefined : decodeControlText(payload.subarray(CLOSING_PREFIX_BYTES));
  return { closeReason, diagnostic };
}

function decodeControlText(value: Uint8Array): string {
  return new TextDecoder('utf-8', { fatal: true }).decode(value);
}

export function splitStreamWireFrames(chunk: Uint8Array): readonly Uint8Array[] {
  if (chunk.length === 0) throw new Error('Stream frame prefix is incomplete.');
  const frames: Uint8Array[] = [];
  let offset = 0;
  while (offset < chunk.length) {
    const remaining = chunk.length - offset;
    if (remaining < ZLINK_STREAM_FRAME_PREFIX_BYTES) {
      throw new Error('Stream frame prefix is incomplete.');
    }
    const headerLength = readUInt16BE(chunk, offset);
    const payloadLength = readUInt32BE(chunk, offset + UINT16_BYTES);
    const frameLength = ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength + payloadLength;
    if (frameLength > remaining) throw new Error('Frame length does not match prefix.');
    frames.push(chunk.subarray(offset, offset + frameLength));
    offset += frameLength;
  }
  return frames;
}

export const ZlinkStreamControlPacket = Object.freeze({
  HeartbeatPing: '$zlink.heartbeat.ping',
  HeartbeatPong: '$zlink.heartbeat.pong',
  ActorBound: '$zlink.actor.bound',
  ActorUnbound: '$zlink.actor.unbound',
  SessionClosing: 'session-closing'
} as const);

export const ZlinkStreamContentType = Object.freeze({
  Raw: 'application/octet-stream',
  Json: 'application/json',
  MessagePack: 'application/x-msgpack',
  Protobuf: 'application/x-protobuf'
} as const);

const contentTypesByCodec: ReadonlyMap<ZlinkStreamCodec, string> = new Map([
  [ZlinkStreamCodec.Raw, ZlinkStreamContentType.Raw],
  [ZlinkStreamCodec.Json, ZlinkStreamContentType.Json],
  [ZlinkStreamCodec.MessagePack, ZlinkStreamContentType.MessagePack],
  [ZlinkStreamCodec.Protobuf, ZlinkStreamContentType.Protobuf]
]);
const codecsByContentType: ReadonlyMap<string, ZlinkStreamCodec> = new Map(
  Array.from(contentTypesByCodec, ([codec, contentType]) => [contentType, codec])
);

export function streamCodecContentType(codec: ZlinkStreamCodec): string {
  const contentType = contentTypesByCodec.get(codec);
  if (contentType === undefined) throw new TypeError(`Unsupported STREAM codec '${codec}'.`);
  return contentType;
}

export function streamCodecForContentType(contentType: string): ZlinkStreamCodec {
  const codec = codecsByContentType.get(contentType);
  if (codec === undefined) throw new TypeError(`Unsupported STREAM content type '${contentType}'.`);
  return codec;
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
  readonly actorSlot?: number;
}

export interface ZLinkStreamWireHeaderFlags {
  readonly hasRequestSeq: number;
  readonly hasMetadata: number;
  readonly hasCorrelationId: number;
  readonly hasFlowId: number;
  readonly hasActorSlot: number;
}

// The functions below resolve this inside their bodies rather than in a parameter
// default. The browser IIFE build of this package is committed into the Unity
// WebGL adapter as an emscripten pre-js, and emscripten's dead-code pass does not
// walk parameter default initializers: a binding whose only references are
// defaults is deleted, and the player fails at runtime with "not defined".
const defaultHeaderFlags: ZLinkStreamWireHeaderFlags = {
  hasRequestSeq: ZlinkStreamHeaderFlags.HasRequestSeq,
  hasMetadata: ZlinkStreamHeaderFlags.HasMetadata,
  hasCorrelationId: ZlinkStreamHeaderFlags.HasCorrelationId,
  hasFlowId: ZlinkStreamHeaderFlags.HasFlowId,
  hasActorSlot: ZlinkStreamHeaderFlags.HasActorSlot
};

export const ZLINK_STREAM_FORMAT_MARKER = 0xf2;

export function encodeStreamWireFrame(header: Uint8Array, payload: Uint8Array): Uint8Array {
  if (header.length > UINT16_MAX) {
    throw new Error('Stream header is too large.');
  }
  if (payload.length > UINT32_MAX) {
    throw new Error('Stream payload is too large.');
  }
  const frame = new Uint8Array(ZLINK_STREAM_FRAME_PREFIX_BYTES + header.length + payload.length);
  writeUInt16BE(frame, 0, header.length);
  writeUInt32BE(frame, UINT16_BYTES, payload.length);
  frame.set(header, ZLINK_STREAM_FRAME_PREFIX_BYTES);
  frame.set(payload, ZLINK_STREAM_FRAME_PREFIX_BYTES + header.length);
  return frame;
}

export function decodeStreamWireFrame(frame: Uint8Array): ZLinkStreamWireFrame {
  if (frame.length < ZLINK_STREAM_FRAME_PREFIX_BYTES) {
    throw new Error('Stream frame prefix is incomplete.');
  }
  const headerLength = readUInt16BE(frame, 0);
  const payloadLength = readUInt32BE(frame, UINT16_BYTES);
  if (frame.length !== ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength + payloadLength) {
    throw new Error('Stream frame length does not match prefix.');
  }
  return {
    header: frame.slice(
      ZLINK_STREAM_FRAME_PREFIX_BYTES,
      ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength
    ),
    payload: frame.slice(ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength)
  };
}

export function tryDecodeStreamWireFrame(frame: Uint8Array): ZLinkStreamWireFrame | undefined {
  if (frame.length < ZLINK_STREAM_FRAME_PREFIX_BYTES) {
    return undefined;
  }
  const headerLength = readUInt16BE(frame, 0);
  const payloadLength = readUInt32BE(frame, UINT16_BYTES);
  if (frame.length !== ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength + payloadLength) {
    return undefined;
  }
  return {
    header: frame.slice(
      ZLINK_STREAM_FRAME_PREFIX_BYTES,
      ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength
    ),
    payload: frame.slice(ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength)
  };
}

export function encodeStreamWireHeader(
  header: ZLinkStreamWireHeader,
  flagOverrides?: ZLinkStreamWireHeaderFlags
): Uint8Array {
  const flags = flagOverrides ?? defaultHeaderFlags;
  const reply = isReplyKind(header.kind);
  const packetName = reply ? '' : header.name;
  const nameBytes = reply ? new Uint8Array() : validateStreamWirePacketName(packetName);
  const hasRequestSeq = header.requestSeq !== undefined;
  const hasMetadata = header.metadata.size > 0;
  const correlationBytes =
    header.correlationId !== undefined && header.correlationId.length > 0
      ? utf8Encode(header.correlationId)
      : undefined;
  if (correlationBytes !== undefined && correlationBytes.length > UINT8_MAX) {
    throw new Error('Stream correlation id is too large.');
  }
  const hasCorrelation = correlationBytes !== undefined;
  const hasFlow = header.flowId !== undefined || header.flowOrigin !== undefined;
  const hasActorSlot = header.actorSlot !== undefined;
  if (hasFlow && (header.flowId === undefined || header.flowOrigin === undefined)) {
    throw new Error('Stream flow id and origin must be provided together.');
  }
  if (header.flowId !== undefined) validateFlowId(header.flowId);
  if (header.flowOrigin !== undefined && ![1, 2, 3, 4].includes(header.flowOrigin)) {
    throw new Error('Stream flow origin is invalid.');
  }
  if (
    header.actorSlot !== undefined &&
    (!Number.isInteger(header.actorSlot) || header.actorSlot < 1 || header.actorSlot > UINT16_MAX)
  ) {
    throw new Error('Stream actor slot is invalid.');
  }
  let headerFlags = header.flags;
  headerFlags = hasRequestSeq
    ? headerFlags | flags.hasRequestSeq
    : headerFlags & ~flags.hasRequestSeq;
  headerFlags = hasMetadata ? headerFlags | flags.hasMetadata : headerFlags & ~flags.hasMetadata;
  headerFlags = hasCorrelation
    ? headerFlags | flags.hasCorrelationId
    : headerFlags & ~flags.hasCorrelationId;
  headerFlags = hasFlow ? headerFlags | flags.hasFlowId : headerFlags & ~flags.hasFlowId;
  headerFlags = hasActorSlot ? headerFlags | flags.hasActorSlot : headerFlags & ~flags.hasActorSlot;

  const metadataBytes = hasMetadata ? encodeStreamWireMetadata(header.metadata) : new Uint8Array();
  const size =
    4 +
    (hasRequestSeq ? UINT64_BYTES : 0) +
    1 +
    nameBytes.length +
    (hasMetadata ? UINT16_BYTES + metadataBytes.length : 0) +
    (hasCorrelation ? 1 + correlationBytes.length : 0) +
    (hasFlow ? FLOW_FIELDS_BYTES : 0) +
    (hasActorSlot ? UINT16_BYTES : 0);
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
    offset += UINT64_BYTES;
  }
  buffer[offset++] = nameBytes.length;
  buffer.set(nameBytes, offset);
  offset += nameBytes.length;
  if (hasMetadata) {
    writeUInt16BE(buffer, offset, metadataBytes.length);
    offset += UINT16_BYTES;
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
    offset += FLOW_ID_BYTES;
    buffer[offset++] = header.flowOrigin!;
  }
  if (hasActorSlot) {
    writeUInt16BE(buffer, offset, header.actorSlot!);
  }
  return buffer;
}

export function decodeStreamWireHeader(
  header: Uint8Array,
  flagOverrides?: ZLinkStreamWireHeaderFlags,
  includeFlow = true
): ZLinkStreamWireHeader {
  const flags = flagOverrides ?? defaultHeaderFlags;
  let offset = 0;
  if (header.length < HEADER_MIN_BYTES) {
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
  const hasActorSlot = (headerFlags & flags.hasActorSlot) !== 0;
  if ((headerFlags & ~HEADER_FLAG_MASK) !== 0) {
    throw new Error('Unknown mandatory stream header flag.');
  }
  let requestSeq: bigint | undefined;
  if (hasRequestSeq) {
    if (header.length - offset < UINT64_BYTES) {
      throw new Error('Stream request sequence is incomplete.');
    }
    requestSeq = readBigUInt64BE(header, offset);
    if (requestSeq === 0n) {
      throw new Error('Request sequence must not be zero.');
    }
    offset += UINT64_BYTES;
  }
  if (header.length - offset < 1) {
    throw new Error('Stream packet name length is missing.');
  }
  const nameLength = header[offset++];
  if (
    (isReplyKind(kind) ? nameLength !== 0 : nameLength === 0) ||
    header.length - offset < nameLength
  ) {
    throw new Error('Stream packet name is invalid.');
  }
  const name = utf8Decode(header.subarray(offset, offset + nameLength));
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
    if (header.length - offset < FLOW_FIELDS_BYTES) {
      throw new Error('Stream flow fields are incomplete.');
    }
    if (includeFlow) {
      flowId = asciiDecode(header.subarray(offset, offset + FLOW_ID_BYTES));
      validateFlowId(flowId);
    }
    offset += FLOW_ID_BYTES;
    const decodedFlowOrigin = header[offset++];
    if (includeFlow && ![1, 2, 3, 4].includes(decodedFlowOrigin)) {
      throw new Error('Stream flow origin is invalid.');
    }
    flowOrigin = includeFlow ? decodedFlowOrigin : undefined;
  }
  let actorSlot: number | undefined;
  if (hasActorSlot) {
    if (header.length - offset < UINT16_BYTES) {
      throw new Error('Stream actor slot is incomplete.');
    }
    actorSlot = readUInt16BE(header, offset);
    if (actorSlot === 0) {
      throw new Error('Stream actor slot must not be zero.');
    }
    offset += UINT16_BYTES;
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
    flowOrigin,
    actorSlot
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

/** Standalone metadata includes its count byte even when the map is empty. */
export function streamWireMetadataSize(metadata: ReadonlyMap<string, string>): number {
  if (metadata.size > UINT8_MAX) {
    throw new Error('Metadata entry count must not exceed 255.');
  }
  let size = 1;
  for (const [key, value] of metadata) {
    const keySize = utf8Size(key);
    const valueSize = utf8Size(value);
    if (keySize === 0 || keySize > UINT8_MAX) {
      throw new Error('Metadata key length is invalid.');
    }
    if (valueSize > UINT16_MAX) {
      throw new Error('Metadata value is too large.');
    }
    size += 1 + keySize + UINT16_BYTES + valueSize;
  }
  return size;
}

export function encodeStreamWireMetadata(metadata: ReadonlyMap<string, string>): Uint8Array {
  const buffer = new Uint8Array(streamWireMetadataSize(metadata));
  let offset = 0;
  buffer[offset++] = metadata.size;
  for (const [key, value] of metadata) {
    const keyBytes = utf8Encode(key);
    const valueBytes = utf8Encode(value);
    buffer[offset++] = keyBytes.length;
    buffer.set(keyBytes, offset);
    offset += keyBytes.length;
    writeUInt16BE(buffer, offset, valueBytes.length);
    offset += UINT16_BYTES;
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

export function lz4PickledLength(payload: Uint8Array): number {
  return pickledLength(payload);
}

export function lz4UnpicklePayload(payload: Uint8Array, maxDecompressedSize?: number): Uint8Array {
  return unpicklePayload(payload, maxDecompressedSize ?? defaultMaxDecompressedPayloadSize);
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
  if (header.length - offset < UINT16_BYTES) {
    throw new Error('Stream metadata section is incomplete.');
  }
  const metadataLength = readUInt16BE(header, offset);
  offset += UINT16_BYTES;
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
    offset += UINT16_BYTES;
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

function validateStreamWirePacketName(name: string): Uint8Array {
  const nameBytes = utf8Encode(name);
  if (name.trim().length === 0 || nameBytes.length > ZLINK_STREAM_MAX_PACKET_NAME_BYTES) {
    throw new Error('Stream packet name is invalid.');
  }
  return nameBytes;
}

function isReplyKind(kind: number): boolean {
  return kind === ZlinkStreamMessageKind.Response || kind === ZlinkStreamMessageKind.Error;
}

function writeUInt16BE(buffer: Uint8Array, offset: number, value: number): void {
  buffer[offset] = (value >>> 8) & UINT8_MAX;
  buffer[offset + 1] = value & UINT8_MAX;
}

function readUInt16BE(buffer: Uint8Array, offset: number): number {
  return (buffer[offset] << 8) | buffer[offset + 1];
}

function readUInt32BE(buffer: Uint8Array, offset: number): number {
  return (
    buffer[offset] * 0x1000000 +
    ((buffer[offset + 1] << 16) | (buffer[offset + 2] << 8) | buffer[offset + 3])
  );
}

function writeUInt32BE(buffer: Uint8Array, offset: number, value: number): void {
  buffer[offset] = (value >>> 24) & UINT8_MAX;
  buffer[offset + 1] = (value >>> 16) & UINT8_MAX;
  buffer[offset + 2] = (value >>> 8) & UINT8_MAX;
  buffer[offset + 3] = value & UINT8_MAX;
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

/** TextEncoder replaces lone surrogates with the three-byte replacement character. */
function utf8Size(value: string): number {
  let size = 0;
  for (let index = 0; index < value.length; index += 1) {
    const code = value.charCodeAt(index);
    if (code < UTF8_TWO_BYTE_MIN) size += 1;
    else if (code < UTF8_THREE_BYTE_MIN) size += 2;
    else if (
      code >= UTF16_HIGH_SURROGATE_MIN &&
      code <= UTF16_HIGH_SURROGATE_MAX &&
      index + 1 < value.length &&
      value.charCodeAt(index + 1) >= UTF16_LOW_SURROGATE_MIN &&
      value.charCodeAt(index + 1) <= UTF16_LOW_SURROGATE_MAX
    ) {
      size += 4;
      index += 1;
    } else size += 3;
  }
  return size;
}
