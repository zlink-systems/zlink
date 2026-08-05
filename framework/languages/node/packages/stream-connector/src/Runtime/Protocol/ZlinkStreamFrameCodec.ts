import { ZlinkStreamErrorCode } from '../../Contracts';
import { decodeStreamWireFrame, encodeStreamWireFrame } from '@zlink-systems/stream-wire';
import { connectorError } from '../ZlinkStreamSupport';

export class ZlinkStreamFrameCodec {
  static encode(header: Uint8Array, payload: Uint8Array, maxPayloadSize = 64 * 1024): Uint8Array {
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
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Frame length does not match prefix.', cause);
    }
  }
}

export function splitZlinkStreamFrames(chunk: Uint8Array): readonly Uint8Array[] {
  if (chunk.length === 0) {
    throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Stream frame prefix is incomplete.');
  }

  const frames: Uint8Array[] = [];
  let offset = 0;
  while (offset < chunk.length) {
    const remaining = chunk.length - offset;
    if (remaining < 6) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Stream frame prefix is incomplete.');
    }
    const headerLength = (chunk[offset] << 8) | chunk[offset + 1];
    const payloadLength = (
      chunk[offset + 2] * 0x1000000
      + (chunk[offset + 3] << 16)
      + (chunk[offset + 4] << 8)
      + chunk[offset + 5]
    );
    const frameLength = 6 + headerLength + payloadLength;
    if (frameLength > remaining) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, 'Frame length does not match prefix.');
    }
    frames.push(chunk.subarray(offset, offset + frameLength));
    offset += frameLength;
  }
  return frames;
}

function validatePayload(payloadLength: number, maxPayloadSize: number): void {
  if (payloadLength > maxPayloadSize) {
    throw connectorError(ZlinkStreamErrorCode.FrameTooLarge, 'Payload exceeds MaxSendPayloadSize.');
  }
}
