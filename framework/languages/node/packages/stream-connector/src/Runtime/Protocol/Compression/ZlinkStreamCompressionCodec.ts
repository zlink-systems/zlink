import {
  type ZlinkStreamCompressionCodec,
  ZlinkStreamCompression,
  ZlinkStreamErrorCode,
  ZlinkStreamException
} from '../../../Contracts';
import { ZlinkStreamHeaderFlags } from '../../../Contracts/ZlinkStreamEnums';
import type { ZlinkStreamHeader } from '../../../Contracts/ZlinkStreamModels';
import {
  lz4PickledLength,
  lz4PickleUncompressed,
  lz4UnpicklePayload
} from '@zlink-systems/stream-wire';
import { connectorError } from '../../ZlinkStreamSupport';

export const zlinkStreamLz4CompressionCodec: ZlinkStreamCompressionCodec = {
  compress(payload) {
    return lz4PickleUncompressed(payload);
  },
  decompress(payload, maxDecompressedSize) {
    // Spec stream-connector 32 §4.7: a result over the receive limit is
    // `FrameTooLarge`. The pickle header declares the result length, so the
    // limit is checked before anything is allocated.
    if (lz4PickledLength(payload) > maxDecompressedSize) {
      throw connectorError(
        ZlinkStreamErrorCode.FrameTooLarge,
        'LZ4 decoded payload exceeds MaxReceivePayloadSize.'
      );
    }
    return lz4UnpicklePayload(payload, maxDecompressedSize);
  }
};

export function compressPayload(
  payload: Uint8Array,
  compression: ZlinkStreamCompression,
  compressionCodec: ZlinkStreamCompressionCodec | undefined
): Uint8Array {
  const codec = resolveCompressionCodec(compression, compressionCodec);
  if (codec === undefined) {
    throw connectorError(
      ZlinkStreamErrorCode.CompressionFailed,
      'Compression codec is not configured.'
    );
  }

  try {
    return codec.compress(payload);
  } catch (cause) {
    throw connectorError(ZlinkStreamErrorCode.CompressionFailed, 'Compression failed.', cause);
  }
}

export function decompressIfNeeded(
  header: ZlinkStreamHeader,
  payload: Uint8Array,
  compression: ZlinkStreamCompression,
  compressionCodec: ZlinkStreamCompressionCodec | undefined,
  maxDecompressedSize: number
): Uint8Array {
  if ((header.flags & ZlinkStreamHeaderFlags.PayloadCompressed) === 0) {
    return payload;
  }
  const codec = resolveCompressionCodec(compression, compressionCodec);
  if (codec === undefined) {
    throw connectorError(
      ZlinkStreamErrorCode.DecompressionFailed,
      'Compression codec is not configured.'
    );
  }

  let decompressed: Uint8Array;
  try {
    decompressed = codec.decompress(payload, maxDecompressedSize);
  } catch (cause) {
    if (cause instanceof ZlinkStreamException) throw cause;
    throw connectorError(ZlinkStreamErrorCode.DecompressionFailed, 'Decompression failed.', cause);
  }
  // A custom codec may not apply the limit it is given, so the result is checked here.
  if (decompressed.length > maxDecompressedSize) {
    throw connectorError(
      ZlinkStreamErrorCode.FrameTooLarge,
      'Decompressed payload exceeds MaxReceivePayloadSize.'
    );
  }
  return decompressed;
}

function resolveCompressionCodec(
  compression: ZlinkStreamCompression,
  compressionCodec: ZlinkStreamCompressionCodec | undefined
): ZlinkStreamCompressionCodec | undefined {
  if (compression === ZlinkStreamCompression.None) {
    return undefined;
  }
  return compressionCodec ?? zlinkStreamLz4CompressionCodec;
}
