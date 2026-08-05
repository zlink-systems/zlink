import {
  type ZlinkStreamCompressionCodec,
  ZlinkStreamCompression,
  ZlinkStreamErrorCode
} from '../../../Contracts';
import { ZlinkStreamHeaderFlags } from '../../../Contracts/ZlinkStreamEnums';
import type { ZlinkStreamHeader } from '../../../Contracts/ZlinkStreamModels';
import { lz4PickleUncompressed, lz4UnpicklePayload } from '@zlink-systems/stream-wire';
import { connectorError } from '../../ZlinkStreamSupport';

export const zlinkStreamLz4CompressionCodec: ZlinkStreamCompressionCodec = {
  compress(payload) {
    return lz4PickleUncompressed(payload);
  },
  decompress(payload, maxDecompressedSize) {
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
    throw connectorError(ZlinkStreamErrorCode.CompressionFailed, 'Compression codec is not configured.');
  }

  return codec.compress(payload);
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
    throw connectorError(ZlinkStreamErrorCode.DecompressionFailed, 'Compression codec is not configured.');
  }

  try {
    const decompressed = codec.decompress(payload, maxDecompressedSize);
    if (decompressed.length > maxDecompressedSize) {
      throw new Error('Decoded payload exceeds MaxReceivePayloadSize.');
    }
    return decompressed;
  } catch (cause) {
    throw connectorError(ZlinkStreamErrorCode.DecompressionFailed, 'Decompression failed.', cause);
  }
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
