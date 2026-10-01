export {
  ZlinkStreamMessageKind as ZLinkStreamMessageKind,
  ZlinkStreamHeaderFlags as ZLinkStreamHeaderFlags,
  ZlinkStreamCloseReasonCode as ZLinkStreamCloseReasonCode
} from '@zlink-systems/stream-wire';
import {
  ZlinkStreamControlPacket,
  ZlinkStreamMessageKind as ZLinkStreamMessageKind,
  ZlinkStreamHeaderFlags as ZLinkStreamHeaderFlags,
  ZlinkStreamCloseReasonCode as ZLinkStreamCloseReasonCode,
  defaultMaxDecompressedPayloadSize,
  encodeStreamWireActorBoundPayload,
  encodeStreamWireActorUnboundPayload,
  encodeStreamWireSessionClosingPayload
} from '@zlink-systems/stream-wire';
import type { Message } from '../../contracts/Common/Message';
import type { ZLinkFlowOrigin } from '../../contracts';
import { resolveFrameworkPacketName } from '../messaging/packet-name';
import {
  ZlinkStreamCodec as ZLinkStreamCodec,
  streamCodecContentType as wireCodecContentType,
  streamCodecForContentType as wireCodecForContentType,
  decodeStreamWireFrame,
  decodeStreamWireHeader,
  encodeStreamWireFrame,
  encodeStreamWireHeader,
  lz4PickleUncompressed,
  lz4UnpicklePayload,
  tryDecodeStreamWireFrame
} from '@zlink-systems/stream-wire';
import { throwAlreadySubmitted } from '../messaging/submission-result';

export { utf8Decode, utf8Encode } from '@zlink-systems/stream-wire';

const actorRequestDeadlineMetadataKey = '$zlink.actor-request-deadline-unix-ms';
//  Shared empty metadata for the dominant no-metadata frame; ReadonlyMap
//  keeps every consumer from mutating it.
const EMPTY_STREAM_METADATA: ReadonlyMap<string, string> = new Map();

export { ZlinkStreamCodec as ZLinkStreamCodec } from '@zlink-systems/stream-wire';

export function streamCodecContentType(codec: ZLinkStreamCodec): string {
  return wireCodecContentType(codec);
}

export function streamCodecForContentType(contentType: string): ZLinkStreamCodec {
  return wireCodecForContentType(contentType);
}

export const ZLINK_STREAM_HEARTBEAT_PING = ZlinkStreamControlPacket.HeartbeatPing;
export const ZLINK_STREAM_HEARTBEAT_PONG = ZlinkStreamControlPacket.HeartbeatPong;

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
  readonly actorSlot?: number;
}

export type ZLinkStreamReplyMessageKind =
  ZLinkStreamMessageKind.Response | ZLinkStreamMessageKind.Error;

export interface ZLinkStreamFrame {
  readonly header: Uint8Array;
  readonly payload: Uint8Array;
}

export function resolvePacketName(
  message: unknown,
  explicitPacketName: string | undefined
): string {
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
  return encodeStreamFrame(
    {
      kind: ZLinkStreamMessageKind.Control,
      codec: ZLinkStreamCodec.Raw,
      flags: ZLinkStreamHeaderFlags.None,
      name,
      metadata: EMPTY_STREAM_METADATA
    },
    new Uint8Array()
  );
}

export function encodeActorBoundFrame(actorSlot: number, actorId: string): Uint8Array {
  return encodeStreamFrame(
    controlHeader(ZlinkStreamControlPacket.ActorBound),
    encodeStreamWireActorBoundPayload(actorSlot, actorId)
  );
}

export function encodeActorUnboundFrame(actorSlot: number): Uint8Array {
  return encodeStreamFrame(
    controlHeader(ZlinkStreamControlPacket.ActorUnbound),
    encodeStreamWireActorUnboundPayload(actorSlot)
  );
}

export function encodeSessionClosingFrame(
  diagnostic = '',
  reason = ZLinkStreamCloseReasonCode.ServerDrain
): Uint8Array {
  return encodeStreamFrame(
    {
      kind: ZLinkStreamMessageKind.Control,
      codec: ZLinkStreamCodec.Raw,
      flags: ZLinkStreamHeaderFlags.None,
      name: ZlinkStreamControlPacket.SessionClosing,
      metadata: new Map()
    },
    encodeStreamWireSessionClosingPayload(reason, diagnostic)
  );
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
  const hasActorSlot = header.actorSlot !== undefined;
  if (
    header.kind === ZLinkStreamMessageKind.Control &&
    (hasCorrelation || hasRequestSeq || hasMetadata || hasFlow || hasActorSlot)
  ) {
    throw new Error(
      'Control packet must not contain a request sequence, metadata, correlation id, or flow id.'
    );
  }
  //  Explicit construction: this runs for every outbound frame, so avoid a
  //  per-message spread of the whole header.
  return encodeStreamWireHeader({
    kind: header.kind,
    codec: header.codec,
    flags: header.flags,
    requestSeq: header.requestSeq,
    name: header.name,
    metadata: header.metadata,
    correlationId: header.correlationId,
    flowId: header.flowId,
    flowOrigin: encodeFlowOrigin(header.flowOrigin),
    actorSlot: header.actorSlot
  });
}

export function decodeStreamHeader(header: Uint8Array, flowEnabled = true): ZLinkStreamFrameHeader {
  const decoded = decodeStreamWireHeader(header, undefined, flowEnabled);
  const kind = decoded.kind as ZLinkStreamMessageKind;
  const flags = decoded.flags as ZLinkStreamHeaderFlags;
  const hasRequestSeq = (flags & ZLinkStreamHeaderFlags.HasRequestSeq) !== 0;
  const hasMetadata = (flags & ZLinkStreamHeaderFlags.HasMetadata) !== 0;
  const hasCorrelation = (flags & ZLinkStreamHeaderFlags.HasCorrelationId) !== 0;
  const hasFlow = (flags & ZLinkStreamHeaderFlags.HasFlowId) !== 0;
  const hasActorSlot = (flags & ZLinkStreamHeaderFlags.HasActorSlot) !== 0;
  if (
    kind === ZLinkStreamMessageKind.Control &&
    (hasCorrelation || hasRequestSeq || hasMetadata || hasFlow || hasActorSlot)
  ) {
    throw new Error(
      'Control packet must not contain a request sequence, metadata, correlation id, or flow id.'
    );
  }
  const metadata = publicStreamMetadata(decoded.metadata);
  return {
    kind,
    codec: decoded.codec as ZLinkStreamCodec,
    flags:
      (metadata.size === 0 ? flags & ~ZLinkStreamHeaderFlags.HasMetadata : flags) &
      (flowEnabled ? ~0 : ~ZLinkStreamHeaderFlags.HasFlowId),
    requestSeq: decoded.requestSeq,
    name: decoded.name,
    metadata,
    correlationId: decoded.correlationId,
    flowId: decoded.flowId,
    flowOrigin: decodeFlowOrigin(decoded.flowOrigin),
    actorSlot: decoded.actorSlot
  };
}

export function actorRequestDeadlineMetadata(
  deadlineUnixMs: number | undefined
): ReadonlyMap<string, string> {
  return deadlineUnixMs === undefined
    ? EMPTY_STREAM_METADATA
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

export function createStreamReplyHeader(
  requestHeader: ZLinkStreamFrameHeader,
  kind: ZLinkStreamReplyMessageKind,
  codec: ZLinkStreamCodec,
  flags: ZLinkStreamHeaderFlags,
  metadata: ReadonlyMap<string, string>,
  includeFlow = true
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
    // correlation_id survives every diagnostics level; the flow pair is
    // observation-only and is not copied into the reply when tracing is Off
    // (spec 27 §4, §7).
    correlationId: requestHeader.correlationId,
    actorSlot: requestHeader.actorSlot,
    ...(includeFlow ? { flowId: requestHeader.flowId, flowOrigin: requestHeader.flowOrigin } : {})
  };
}

function controlHeader(name: string): ZLinkStreamFrameHeader {
  return {
    kind: ZLinkStreamMessageKind.Control,
    codec: ZLinkStreamCodec.Raw,
    flags: ZLinkStreamHeaderFlags.None,
    name,
    metadata: EMPTY_STREAM_METADATA
  };
}

function encodeFlowOrigin(origin: ZLinkFlowOrigin | undefined): number | undefined {
  return origin === undefined
    ? undefined
    : ({ Inbound: 1, Timer: 2, Application: 3, Lifecycle: 4 } as const)[origin];
}

function decodeFlowOrigin(origin: number | undefined): ZLinkFlowOrigin | undefined {
  return origin === undefined
    ? undefined
    : ({ 1: 'Inbound', 2: 'Timer', 3: 'Application', 4: 'Lifecycle' } as const)[origin];
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

export function lz4Unpickle(
  payload: Uint8Array,
  maxDecompressedSize = defaultMaxDecompressedPayloadSize
): Uint8Array {
  return lz4UnpicklePayload(payload, maxDecompressedSize);
}
