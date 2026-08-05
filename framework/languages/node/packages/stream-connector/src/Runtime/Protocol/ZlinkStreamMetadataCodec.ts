import { ZlinkStreamErrorCode, ZlinkStreamMetadata, ZlinkStreamMetadataMap } from '../../Contracts';
import { decodeStreamWireMetadata, encodeStreamWireMetadata } from '@zlink-systems/stream-wire';
import { connectorError } from '../ZlinkStreamSupport';

export const ZLINK_STREAM_MAX_METADATA_BYTES = 1024;

export class ZlinkStreamMetadataCodec {
  static size(metadata: ZlinkStreamMetadata): number {
    try {
      const size = metadata.count === 0 ? 0 : encodeStreamWireMetadata(metadata.values).length;
      if (size > ZLINK_STREAM_MAX_METADATA_BYTES) {
        throw connectorError(
          ZlinkStreamErrorCode.ValidationFailed,
          `Metadata must not exceed ${ZLINK_STREAM_MAX_METADATA_BYTES} bytes.`
        );
      }
      return size;
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, streamWireErrorMessage(cause), cause);
    }
  }

  static write(metadata: ZlinkStreamMetadata, destination: Uint8Array): void {
    try {
      destination.set(encodeStreamWireMetadata(metadata.values));
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.ValidationFailed, streamWireErrorMessage(cause), cause);
    }
  }

  static decode(metadata: Uint8Array): ZlinkStreamMetadata {
    try {
      const values = decodeStreamWireMetadata(metadata);
      return values.size === 0 ? ZlinkStreamMetadataMap.empty : ZlinkStreamMetadataMap.from(values);
    } catch (cause) {
      throw connectorError(ZlinkStreamErrorCode.FrameDecodeFailed, streamWireErrorMessage(cause), cause);
    }
  }
}

function streamWireErrorMessage(cause: unknown): string {
  return cause instanceof Error ? cause.message : 'Metadata is invalid.';
}
