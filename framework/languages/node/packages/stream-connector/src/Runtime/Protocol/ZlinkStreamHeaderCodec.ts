import {
  ZlinkStreamCodec,
  ZlinkStreamErrorCode,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadata,
  ZlinkStreamMetadataMap
} from '../../Contracts';
import { ZlinkStreamHeaderFlags } from '../../Contracts/ZlinkStreamEnums';
import type { ZlinkStreamHeader } from '../../Contracts/ZlinkStreamModels';
import { decodeStreamWireHeader, encodeStreamWireHeader } from '@zlink-systems/stream-wire';
import { connectorError } from '../ZlinkStreamSupport';
import { validateName } from './ZlinkStreamPacketNameValidator';
import { ZlinkStreamMetadataCodec } from './ZlinkStreamMetadataCodec';

export class ZlinkStreamHeaderCodec {
  static encode(header: ZlinkStreamHeader): Uint8Array {
    const reply = isReplyKind(header.kind);
    if (!reply) validateName(header.name, header.kind === ZlinkStreamMessageKind.Control);
    validateHeaderSemantics(header);
    ZlinkStreamMetadataCodec.size(header.metadata);
    try {
      return encodeStreamWireHeader({
        kind: header.kind,
        codec: header.codec,
        flags: header.flags,
        requestSeq: header.requestSeq,
        name: header.name,
        metadata: header.metadata.values,
        correlationId: header.correlationId,
        flowId: header.flowId,
        flowOrigin: encodeFlowOrigin(header.flowOrigin)
      });
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, streamWireErrorMessage(cause), cause);
    }
  }

  static decode(header: Uint8Array): ZlinkStreamHeader {
    let decoded: ZlinkStreamHeader;
    try {
      const wire = decodeStreamWireHeader(header);
      const metadata = wire.metadata.size === 0
        ? ZlinkStreamMetadataMap.empty
        : ZlinkStreamMetadataMap.from(wire.metadata);
      decoded = {
        kind: wire.kind as ZlinkStreamMessageKind,
        codec: wire.codec as ZlinkStreamCodec,
        flags: wire.flags as ZlinkStreamHeaderFlags,
        requestSeq: wire.requestSeq,
        name: wire.name,
        metadata,
        correlationId: wire.correlationId,
        flowId: wire.flowId,
        flowOrigin: decodeFlowOrigin(wire.flowOrigin)
      };
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, streamWireErrorMessage(cause), cause);
    }
    validateEnum(decoded.kind, decoded.codec, decoded.flags);
    if (!isReplyKind(decoded.kind)) {
      validateName(decoded.name, decoded.kind === ZlinkStreamMessageKind.Control);
    }
    validateHeaderSemantics(decoded);
    return decoded;
  }
}

function isReplyKind(kind: ZlinkStreamMessageKind): boolean {
  return kind === ZlinkStreamMessageKind.Response || kind === ZlinkStreamMessageKind.Error;
}

function streamWireErrorMessage(cause: unknown): string {
  return cause instanceof Error ? cause.message : 'Stream header is invalid.';
}

export function buildHeader(
  kind: ZlinkStreamMessageKind,
  name: string,
  codec: ZlinkStreamCodec,
  metadata: ZlinkStreamMetadata,
  compress: boolean,
  requestSeq: bigint | undefined,
  correlationId?: string,
  flowId?: string,
  flowOrigin?: import('../../Contracts').ZlinkFlowOrigin
): ZlinkStreamHeader {
  let flags = ZlinkStreamHeaderFlags.None;
  if (requestSeq !== undefined) {
    flags |= ZlinkStreamHeaderFlags.HasRequestSeq;
  }
  if (metadata.count > 0) {
    flags |= ZlinkStreamHeaderFlags.HasMetadata;
  }
  if (compress) {
    flags |= ZlinkStreamHeaderFlags.PayloadCompressed;
  }
  if (correlationId !== undefined && correlationId.length > 0) {
    flags |= ZlinkStreamHeaderFlags.HasCorrelationId;
  }
  if (flowId !== undefined) flags |= ZlinkStreamHeaderFlags.HasFlowId;
  return { kind, codec, flags, requestSeq, name, metadata, correlationId, flowId, flowOrigin };
}

function validateHeaderSemantics(header: ZlinkStreamHeader): void {
  validateEnum(header.kind, header.codec, header.flags);
  const hasRequestSeq = header.requestSeq !== undefined || (header.flags & ZlinkStreamHeaderFlags.HasRequestSeq) !== 0;
  const hasMetadata = header.metadata.count > 0 || (header.flags & ZlinkStreamHeaderFlags.HasMetadata) !== 0;
  if (header.kind === ZlinkStreamMessageKind.Send && hasRequestSeq) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Send packet must not contain a request sequence.');
  }
  if ((header.kind === ZlinkStreamMessageKind.Request || header.kind === ZlinkStreamMessageKind.Response) && !hasRequestSeq) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Request and response packets must contain a request sequence.');
  }
  if (header.kind === ZlinkStreamMessageKind.Error && header.codec !== ZlinkStreamCodec.Json) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Error packet must use the JSON codec.');
  }
  if (header.kind === ZlinkStreamMessageKind.Control) {
    const hasCorrelation = (header.correlationId !== undefined && header.correlationId.length > 0)
      || (header.flags & ZlinkStreamHeaderFlags.HasCorrelationId) !== 0;
    const hasFlow = header.flowId !== undefined || header.flowOrigin !== undefined
      || (header.flags & ZlinkStreamHeaderFlags.HasFlowId) !== 0;
    if (header.flags !== ZlinkStreamHeaderFlags.None || hasRequestSeq || hasMetadata || hasCorrelation || hasFlow || header.codec !== ZlinkStreamCodec.Raw) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Control packet must use raw codec and must not contain flags.');
    }
  }
}

function validateEnum(kind: ZlinkStreamMessageKind, codec: ZlinkStreamCodec, flags: ZlinkStreamHeaderFlags): void {
  if (![1, 2, 3, 4, 5].includes(kind)) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Unknown stream message kind.');
  }
  if (![0, 1, 2, 3].includes(codec)) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Unknown stream codec.');
  }
  const known = ZlinkStreamHeaderFlags.HasRequestSeq
    | ZlinkStreamHeaderFlags.HasMetadata
    | ZlinkStreamHeaderFlags.PayloadCompressed
    | ZlinkStreamHeaderFlags.HasCorrelationId
    | ZlinkStreamHeaderFlags.HasFlowId;
  if ((flags & ~known) !== 0) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Unknown stream header flag.');
  }
}

function encodeFlowOrigin(origin: import('../../Contracts').ZlinkFlowOrigin | undefined): number | undefined {
  return origin === undefined ? undefined : ({ Inbound: 1, Timer: 2, Application: 3, Lifecycle: 4 } as const)[origin];
}

function decodeFlowOrigin(origin: number | undefined): import('../../Contracts').ZlinkFlowOrigin | undefined {
  return origin === undefined ? undefined : ({ 1: 'Inbound', 2: 'Timer', 3: 'Application', 4: 'Lifecycle' } as const)[origin];
}
