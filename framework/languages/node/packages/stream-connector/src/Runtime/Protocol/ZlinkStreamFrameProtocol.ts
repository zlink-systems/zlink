import {
  ZLINK_STREAM_FRAME_PREFIX_BYTES,
  ZlinkStreamControlPacket
} from '@zlink-systems/stream-wire';
import {
  type RequiredZlinkStreamConnectorOptions,
  type ZlinkStreamEncodedPayload,
  type ZlinkStreamMetadata,
  ZlinkStreamCodec,
  ZlinkStreamErrorCode,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadataMap
} from '../../Contracts';

import { ZlinkStreamHeaderFlags } from '../../Contracts/ZlinkStreamEnums';
import type { ZlinkStreamHeader } from '../../Contracts/ZlinkStreamModels';
import { connectorError } from '../ZlinkStreamSupport';
import { compressPayload, decompressIfNeeded } from './Compression/ZlinkStreamCompressionCodec';
import { splitZlinkStreamFrames, ZlinkStreamFrameCodec } from './ZlinkStreamFrameCodec';
import { buildHeader, ZlinkStreamHeaderCodec } from './ZlinkStreamHeaderCodec';
import { ADMISSION_TICKET } from './ZlinkStreamAdmissionQueue';

export const ZLINK_STREAM_HEARTBEAT_PING = ZlinkStreamControlPacket.HeartbeatPing;
export const ZLINK_STREAM_HEARTBEAT_PONG = ZlinkStreamControlPacket.HeartbeatPong;

export class ZlinkStreamFrameProtocol {
  constructor(private readonly options: RequiredZlinkStreamConnectorOptions) {}

  encode(
    kind: ZlinkStreamMessageKind,
    name: string,
    payload: ZlinkStreamEncodedPayload,
    metadata: ZlinkStreamMetadata,
    compress: boolean,
    requestSeq: bigint | undefined,
    correlationId?: string,
    actorSlot?: number
  ): Uint8Array {
    const payloadBytes = compress
      ? compressPayload(payload.payload, this.options.compression, this.options.compressionCodec)
      : payload.payload;
    const header = buildHeader(
      kind,
      name,
      payload.codec,
      metadata,
      compress,
      requestSeq,
      correlationId,
      actorSlot
    );
    return this.encodeFrame(header, payloadBytes);
  }

  reencodeRequest(frame: Uint8Array, requestSeq: bigint, ticket?: string): Uint8Array {
    // The first write already encoded the header and compressed the payload if requested.
    // Use views of this locally encoded frame so the payload is copied only into the new frame.
    const headerLength = new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getUint16(
      0
    );
    const payloadOffset = ZLINK_STREAM_FRAME_PREFIX_BYTES + headerLength;
    const original = frame.subarray(ZLINK_STREAM_FRAME_PREFIX_BYTES, payloadOffset);
    const header =
      ticket === undefined
        ? ZlinkStreamHeaderCodec.encodeRetryHeader(original, requestSeq)
        : this.encodeTicketHeader(original, requestSeq, ticket);
    return ZlinkStreamFrameCodec.encode(
      header,
      frame.subarray(payloadOffset),
      this.options.maxSendPayloadSize
    );
  }

  private encodeTicketHeader(original: Uint8Array, requestSeq: bigint, ticket: string): Uint8Array {
    const header = ZlinkStreamHeaderCodec.decode(original);
    return ZlinkStreamHeaderCodec.encode({
      ...header,
      requestSeq,
      flags: header.flags | ZlinkStreamHeaderFlags.HasMetadata,
      metadata: header.metadata.with(ADMISSION_TICKET, ticket)
    });
  }

  encodeControl(name: string, payload = new Uint8Array()): Uint8Array {
    return this.encodeFrame(
      {
        kind: ZlinkStreamMessageKind.Control,
        codec: ZlinkStreamCodec.Raw,
        flags: ZlinkStreamHeaderFlags.None,
        name,
        metadata: ZlinkStreamMetadataMap.empty
      },
      payload
    );
  }

  decode(frameBytes: Uint8Array): {
    readonly header: ZlinkStreamHeader;
    readonly payload: Uint8Array;
  } {
    const frame = ZlinkStreamFrameCodec.decode(frameBytes);
    return {
      header: ZlinkStreamHeaderCodec.decode(frame.header),
      payload: frame.payload
    };
  }

  decodeFrames(chunk: Uint8Array): readonly {
    readonly header: ZlinkStreamHeader;
    readonly payload: Uint8Array;
  }[] {
    return splitZlinkStreamFrames(chunk).map((frame) => {
      const decoded = this.decode(frame);
      if (decoded.payload.length > this.options.maxReceivePayloadSize) {
        throw connectorError(
          ZlinkStreamErrorCode.FrameTooLarge,
          'Payload exceeds MaxReceivePayloadSize.'
        );
      }
      return decoded;
    });
  }

  decodePayload(header: ZlinkStreamHeader, payload: Uint8Array): Uint8Array {
    return decompressIfNeeded(
      header,
      payload,
      this.options.compression,
      this.options.compressionCodec,
      this.options.maxReceivePayloadSize
    );
  }

  private encodeFrame(header: ZlinkStreamHeader, payload: Uint8Array): Uint8Array {
    return ZlinkStreamFrameCodec.encode(
      ZlinkStreamHeaderCodec.encode(header),
      payload,
      this.options.maxSendPayloadSize
    );
  }
}
