import { ZlinkStreamErrorCode, ZlinkStreamMetadata } from '../../Contracts';
import { streamWireMetadataSize } from '@zlink-systems/stream-wire';
import { connectorError } from '../ZlinkStreamSupport';

export const ZLINK_STREAM_MAX_METADATA_BYTES = 1024;

export class ZlinkStreamMetadataCodec {
  static size(metadata: ZlinkStreamMetadata): number {
    let size: number;
    try {
      // An empty map omits the header section; standalone wire metadata still has a count byte.
      size = metadata.count === 0 ? 0 : streamWireMetadataSize(metadata.values);
    } catch (cause) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        cause instanceof Error ? cause.message : 'Metadata is invalid.',
        cause
      );
    }
    if (size > ZLINK_STREAM_MAX_METADATA_BYTES) {
      throw connectorError(
        ZlinkStreamErrorCode.ValidationFailed,
        `Metadata must not exceed ${ZLINK_STREAM_MAX_METADATA_BYTES} bytes.`
      );
    }
    return size;
  }
}
