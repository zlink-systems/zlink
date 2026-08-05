import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFlowOrigin } from '../../contracts';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  decodeStreamWireFrame,
  decodeStreamWireHeader,
  encodeStreamWireFrame,
  encodeStreamWireHeader,
  lz4PickleUncompressed,
  lz4UnpicklePayload,
  tryDecodeStreamWireFrame,
  utf8Encode
} from '@zlink-systems/stream-wire';
import { throwAlreadySubmitted } from '../messaging/submission-result';

export { utf8Decode, utf8Encode } from '@zlink-systems/stream-wire';

const defaultMaxDecompressedPayloadSize = 64 * 1024;
const actorRequestDeadlineMetadataKey = '$zlink.actor-request-deadline-unix-ms';

export enum ZLinkStreamCodec {
  Raw = 0,
  Json = 1,
  MessagePack = 2,
  Protobuf = 3
}

export enum ZLinkStreamMessageKind {
  Send = 1,
  Request = 2,
  Response = 3,
  Error = 4,
  Control = 5
}

export enum ZLinkStreamHeaderFlags {
  None = 0,
  HasRequestSeq = 0x01,
  HasMetadata = 0x02,
  PayloadCompressed = 0x04,
  HasCorrelationId = 0x08,
  HasFlowId = 0x10
}

export enum ZLinkStreamCloseReasonCode {
  ClientClose = 1,
  IdleTimeout = 2,
  HeartbeatTimeout = 3,
  ServerDrain = 4,
  ProtocolError = 5,
  TransportError = 6
}

export const ZLINK_STREAM_HEARTBEAT_PING = '$zlink.heartbeat.ping';
export const ZLINK_STREAM_HEARTBEAT_PONG = '$zlink.heartbeat.pong';

export interface ZLinkStreamFrameHeader {
  readonly kind: ZLinkStreamMessageKind;
  readonly codec: ZLinkStreamCodec;
  readonly flags: ZLinkStreamHeaderFlags;
  readonly requestSeq?: bigint;
  readonly name: string;
  readonly metadata: ReadonlyMap<string, string>;
  readonly correlationId?: string;
  readonly flowId?: string;
  readonly flowOrigin?: ZLinkFlowOrigin;
}

export type ZLinkStreamReplyMessageKind =
  | ZLinkStreamMessageKind.Response
  | ZLinkStreamMessageKind.Error;

export interface ZLinkStreamFrame {
  readonly header: Uint8Array;
  readonly payload: Uint8Array;
}

export function resolvePacketName(message: unknown, explicitPacketName: string | undefined): string {
  const packetName = resolveFrameworkPacketName(message, explicitPacketName, 'Stream');
  if (packetName.trim().length === 0) {
    throw new Error('Stream packet name must not be empty.');
  }
  return packetName;
}

export function ensureSingleSubmit(executed: boolean): void {
  if (executed) {
    throwAlreadySubmitted('Stream send call');
  }
}

export function encodeStreamFrame(header: ZLinkStreamFrameHeader, payload: Uint8Array): Uint8Array {
  return encodeStreamWireFrame(encodeStreamHeader(header), payload);
}

export function encodeStreamControlFrame(name: string): Uint8Array {
  return encodeStreamFrame({
    kind: ZLinkStreamMessageKind.Control,
    codec: ZLinkStreamCodec.Raw,
    flags: ZLinkStreamHeaderFlags.None,
    name,
    metadata: new Map()
  }, new Uint8Array());
}

export function encodeSessionClosingFrame(
  diagnostic = '',
  reason = ZLinkStreamCloseReasonCode.ServerDrain
): Uint8Array {
  const diagnosticBytes = utf8Encode(diagnostic);
  if (diagnosticBytes.length > 512) throw new Error('Session-closing diagnostic is too large.');
  const payload = new Uint8Array(4 + diagnosticBytes.length);
  payload[0] = 1;
  payload[1] = reason;
  payload[2] = diagnosticBytes.length >>> 8;
  payload[3] = diagnosticBytes.length & 0xff;
  payload.set(diagnosticBytes, 4);
  return encodeStreamFrame({
    kind: ZLinkStreamMessageKind.Control,
    codec: ZLinkStreamCodec.Raw,
    flags: ZLinkStreamHeaderFlags.None,
    name: 'session-closing',
    metadata: new Map()
  }, payload);
}

export function decodeStreamFrame(frame: Uint8Array): ZLinkStreamFrame {
  return decodeStreamWireFrame(frame);
}

export function tryDecodeStreamFrame(frame: Uint8Array): ZLinkStreamFrame | undefined {
  return tryDecodeStreamWireFrame(frame);
}

export function encodeStreamHeader(header: ZLinkStreamFrameHeader): Uint8Array {
  const hasRequestSeq = header.requestSeq !== undefined;
  const hasMetadata = header.metadata.size > 0;
  const hasCorrelation = header.correlationId !== undefined && header.correlationId.length > 0;
  const hasFlow = header.flowId !== undefined || header.flowOrigin !== undefined;
  if (header.kind === ZLinkStreamMessageKind.Control && (hasCorrelation || hasRequestSeq || hasMetadata || hasFlow)) {
    throw new Error('Control packet must not contain a request sequence, metadata, correlation id, or flow id.');
  }
  return encodeStreamWireHeader({ ...header, flowOrigin: encodeFlowOrigin(header.flowOrigin) });
}

export function decodeStreamHeader(header: Uint8Array): ZLinkStreamFrameHeader {
  const decoded = decodeStreamWireHeader(header);
  const kind = decoded.kind as ZLinkStreamMessageKind;
  const flags = decoded.flags as ZLinkStreamHeaderFlags;
  const hasRequestSeq = (flags & ZLinkStreamHeaderFlags.HasRequestSeq) !== 0;
  const hasMetadata = (flags & ZLinkStreamHeaderFlags.HasMetadata) !== 0;
  const hasCorrelation = (flags & ZLinkStreamHeaderFlags.HasCorrelationId) !== 0;
  const hasFlow = (flags & ZLinkStreamHeaderFlags.HasFlowId) !== 0;
  if (kind === ZLinkStreamMessageKind.Control && (hasCorrelation || hasRequestSeq || hasMetadata || hasFlow)) {
    throw new Error('Control packet must not contain a request sequence, metadata, correlation id, or flow id.');
  }
  const metadata = publicStreamMetadata(decoded.metadata);
  return {
    kind,
    codec: decoded.codec as ZLinkStreamCodec,
    flags: metadata.size === 0
      ? flags & ~ZLinkStreamHeaderFlags.HasMetadata
      : flags,
    requestSeq: decoded.requestSeq,
    name: decoded.name,
    metadata,
    correlationId: decoded.correlationId,
    flowId: decoded.flowId,
    flowOrigin: decodeFlowOrigin(decoded.flowOrigin)
  };
}

export function actorRequestDeadlineMetadata(deadlineUnixMs: number | undefined): ReadonlyMap<string, string> {
  return deadlineUnixMs === undefined
    ? new Map()
    : new Map([[actorRequestDeadlineMetadataKey, String(deadlineUnixMs)]]);
}

export function decodeActorRequestDeadlineUnixMs(header: Uint8Array): number | undefined {
  const value = decodeStreamWireHeader(header).metadata.get(actorRequestDeadlineMetadataKey);
  if (value === undefined) return undefined;
  const deadline = Number(value);
  return Number.isSafeInteger(deadline) && deadline > 0 ? deadline : undefined;
}

function publicStreamMetadata(metadata: ReadonlyMap<string, string>): ReadonlyMap<string, string> {
  if (!metadata.has(actorRequestDeadlineMetadataKey)) return metadata;
  const visible = new Map(metadata);
  visible.delete(actorRequestDeadlineMetadataKey);
  return visible;
}

export function tryGetStreamFrameHeader(header: unknown): ZLinkStreamFrameHeader | undefined {
  if (typeof header !== 'object' || header === null) {
    return undefined;
  }
  const value = header as {
    kind?: unknown;
    codec?: unknown;
    flags?: unknown;
    requestSeq?: unknown;
    name?: unknown;
    metadata?: { values?: ReadonlyMap<string, string> };
    correlationId?: unknown;
    flowId?: unknown;
    flowOrigin?: unknown;
  };
  if (
    typeof value.kind !== 'number'
    || typeof value.codec !== 'number'
    || typeof value.flags !== 'number'
    || typeof value.name !== 'string'
  ) {
    return undefined;
  }
  return {
    kind: value.kind as ZLinkStreamMessageKind,
    codec: value.codec as ZLinkStreamCodec,
    flags: value.flags as ZLinkStreamHeaderFlags,
    requestSeq: typeof value.requestSeq === 'bigint' ? value.requestSeq : undefined,
    name: value.name,
    metadata: value.metadata?.values ?? new Map(),
    correlationId: typeof value.correlationId === 'string' ? value.correlationId : undefined,
    flowId: typeof value.flowId === 'string' ? value.flowId : undefined,
    flowOrigin: isFlowOrigin(value.flowOrigin) ? value.flowOrigin : undefined
  };
}

export function requireStreamFrameHeader(header: unknown): ZLinkStreamFrameHeader {
  const parsed = tryGetStreamFrameHeader(header);
  if (parsed === undefined) {
    throw new Error('Stream relay requires a decoded stream header.');
  }
  return parsed;
}

export function createStreamReplyHeader(
  requestHeader: ZLinkStreamFrameHeader,
  kind: ZLinkStreamReplyMessageKind,
  codec: ZLinkStreamCodec,
  flags: ZLinkStreamHeaderFlags,
  metadata: ReadonlyMap<string, string>
): ZLinkStreamFrameHeader {
  if (requestHeader.requestSeq === undefined) {
    throw new Error('Stream reply requires a request sequence.');
  }
  return {
    kind,
    codec,
    flags,
    requestSeq: requestHeader.requestSeq,
    name: '',
    metadata,
    correlationId: requestHeader.correlationId,
    flowId: requestHeader.flowId,
    flowOrigin: requestHeader.flowOrigin
  };
}

function encodeFlowOrigin(origin: ZLinkFlowOrigin | undefined): number | undefined {
  return origin === undefined ? undefined : ({ Inbound: 1, Timer: 2, Application: 3, Lifecycle: 4 } as const)[origin];
}

function decodeFlowOrigin(origin: number | undefined): ZLinkFlowOrigin | undefined {
  return origin === undefined ? undefined : ({ 1: 'Inbound', 2: 'Timer', 3: 'Application', 4: 'Lifecycle' } as const)[origin];
}

function isFlowOrigin(origin: unknown): origin is ZLinkFlowOrigin {
  return origin === 'Inbound' || origin === 'Timer' || origin === 'Application' || origin === 'Lifecycle';
}

export function messageToBytes(message: Message): Uint8Array {
  const value = message as unknown as {
    data?: () => Uint8Array;
    bytes?: Uint8Array;
    toBytes?: () => Uint8Array;
  };
  if (value.data !== undefined) {
    return value.data();
  }
  if (value.bytes !== undefined) {
    return value.bytes;
  }
  if (value.toBytes !== undefined) {
    return value.toBytes();
  }
  throw new Error('Stream payload cannot be copied for relay.');
}

export function lz4Pickle(payload: Uint8Array): Uint8Array {
  return lz4PickleUncompressed(payload);
}

export function lz4Unpickle(payload: Uint8Array, maxDecompressedSize = defaultMaxDecompressedPayloadSize): Uint8Array {
  return lz4UnpicklePayload(payload, maxDecompressedSize);
}
