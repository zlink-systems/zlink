import {
  decodeStreamWireFrame,
  encodeStreamWireFrame,
  splitStreamWireFrames
} from '@zlink-systems/stream-wire';
import { ZlinkStreamErrorCode } from '../../Contracts';
import { ZLINK_STREAM_DEFAULT_PAYLOAD_SIZE } from '../ZlinkStreamConnectorOptions';
import { connectorError } from '../ZlinkStreamSupport';

export class ZlinkStreamFrameCodec {
  static encode(
    header: Uint8Array,
    payload: Uint8Array,
    maxPayloadSize = ZLINK_STREAM_DEFAULT_PAYLOAD_SIZE
  ): Uint8Array {
    validatePayload(payload.length, maxPayloadSize);
    try {
      return encodeStreamWireFrame(header, payload);
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.FrameTooLarge, 'Frame is too large.', cause);
    }
  }

  static decode(frame: Uint8Array): { header: Uint8Array; payload: Uint8Array } {
    try {
      return decodeStreamWireFrame(frame);
    } catch (cause) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameDecodeFailed,
        'Frame length does not match prefix.',
        cause
      );
    }
  }
}

export function splitZlinkStreamFrames(chunk: Uint8Array): readonly Uint8Array[] {
  try {
    return splitStreamWireFrames(chunk);
  } catch (cause) {
    throw connectorError(
      ZlinkStreamErrorCode.FrameDecodeFailed,
      cause instanceof Error ? cause.message : 'Frame length does not match prefix.'
    );
  }
}

function validatePayload(payloadLength: number, maxPayloadSize: number): void {
  if (payloadLength > maxPayloadSize) {
    throw connectorError(
      ZlinkStreamErrorCode.ValidationFailed,
      'Payload exceeds MaxSendPayloadSize.'
    );
  }
}
