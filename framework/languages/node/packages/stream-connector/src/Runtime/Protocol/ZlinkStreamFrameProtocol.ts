import type {
  RequiredZlinkStreamConnectorOptions,
  ZlinkStreamEncodedPayload,
  ZlinkStreamMetadata
} from '../../Contracts';
import {
  ZlinkStreamCodec,
  ZlinkStreamErrorCode,
  ZlinkStreamMessageKind,
  ZlinkStreamMetadataMap
} from '../../Contracts';
import { ZlinkStreamHeaderFlags } from '../../Contracts/ZlinkStreamEnums';
import type { ZlinkStreamHeader } from '../../Contracts/ZlinkStreamModels';
import type { ZlinkFlowOrigin } from '../../Contracts';
import { connectorError } from '../ZlinkStreamSupport';
import {
  compressPayload,
  decompressIfNeeded
} from './Compression/ZlinkStreamCompressionCodec';
import { splitZlinkStreamFrames, ZlinkStreamFrameCodec } from './ZlinkStreamFrameCodec';
import { buildHeader, ZlinkStreamHeaderCodec } from './ZlinkStreamHeaderCodec';

export const ZLINK_STREAM_HEARTBEAT_PING = '$zlink.heartbeat.ping';
export const ZLINK_STREAM_HEARTBEAT_PONG = '$zlink.heartbeat.pong';

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
    flowId?: string,
    flowOrigin?: ZlinkFlowOrigin
  ): Uint8Array {
    const payloadBytes = compress
      ? compressPayload(payload.payload, this.options.compression, this.options.compressionCodec)
      : payload.payload;
    const header = buildHeader(kind, name, payload.codec, metadata, compress, requestSeq, correlationId, flowId, flowOrigin);
    return this.encodeFrame(header, payloadBytes);
  }

  encodeControl(name: string, payload = new Uint8Array()): Uint8Array {
    return this.encodeFrame({
      kind: ZlinkStreamMessageKind.Control,
      codec: ZlinkStreamCodec.Raw,
      flags: ZlinkStreamHeaderFlags.None,
      name,
      metadata: ZlinkStreamMetadataMap.empty
    }, payload);
  }

  decode(frameBytes: Uint8Array): { readonly header: ZlinkStreamHeader; readonly payload: Uint8Array } {
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
        throw connectorError(ZlinkStreamErrorCode.FrameTooLarge, 'Payload exceeds MaxReceivePayloadSize.');
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
