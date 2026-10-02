package systems.zlink.stream.connector;

import net.jpountz.lz4.LZ4Compressor;
import net.jpountz.lz4.LZ4Factory;

import java.util.Arrays;

final class ZLinkStreamLz4Pickler {
    private static final int VERSION_MASK = 0x07;
    private static final int DIFF_WIDTH_SHIFT = 6;
    private static final int DIFF_WIDTH_MASK = 0x03;
    private static final int INTEGER_DIFF_WIDTH_CODE = DIFF_WIDTH_MASK;
    private static final LZ4Factory LZ4 = LZ4Factory.fastestInstance();

    private ZLinkStreamLz4Pickler() {}

    static byte[] pickle(byte[] source) {
        if (source.length == 0) {
            return new byte[0];
        }
        LZ4Compressor compressor = LZ4.fastCompressor();
        byte[] compressed = new byte[compressor.maxCompressedLength(source.length)];
        int compressedLength =
                compressor.compress(source, 0, source.length, compressed, 0, compressed.length);
        if (compressedLength <= 0 || compressedLength >= source.length) {
            byte[] result = new byte[source.length + 1];
            result[0] = 0;
            System.arraycopy(source, 0, result, 1, source.length);
            return result;
        }
        int diffLength = source.length - compressedLength;
        int sizeOfDiff = effectiveSizeOf(diffLength);
        byte[] result = new byte[1 + sizeOfDiff + compressedLength];
        result[0] = encodeHeaderByte(sizeOfDiff);
        pokeLittleEndian(result, 1, diffLength, sizeOfDiff);
        System.arraycopy(compressed, 0, result, 1 + sizeOfDiff, compressedLength);
        return result;
    }

    static byte[] unpickle(byte[] source) {
        return unpickle(source, ZLinkStreamConnectorOptions.DEFAULT_MAX_PAYLOAD_SIZE);
    }

    static byte[] unpickle(byte[] source, int maxDecompressedSize) {
        if (source.length == 0) {
            return new byte[0];
        }
        PickleHeader header = decodeHeader(source);
        if (header.resultLength() > maxDecompressedSize) {
            //  Spec 32 4.7, 9: a decompressed payload over the receive limit is
            //  FrameTooLarge.
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.FRAME_TOO_LARGE,
                    "LZ4 decoded stream payload exceeds maximum stream payload size");
        }
        if (!header.compressed()) {
            return Arrays.copyOfRange(source, header.dataOffset(), source.length);
        }
        int resultLength = Math.toIntExact(header.resultLength());
        byte[] output = new byte[resultLength];
        int decodedLength =
                LZ4.safeDecompressor()
                        .decompress(
                                source,
                                header.dataOffset(),
                                source.length - header.dataOffset(),
                                output,
                                0);
        if (decodedLength != resultLength) {
            throw new IllegalArgumentException(
                    "compressed stream payload decoded to "
                            + decodedLength
                            + " bytes, expected "
                            + resultLength);
        }
        return output;
    }

    private static PickleHeader decodeHeader(byte[] source) {
        int header = Byte.toUnsignedInt(source[0]);
        int version = header & VERSION_MASK;
        if (version != 0) {
            throw new IllegalArgumentException("unsupported LZ4 pickle version: " + version);
        }
        int sizeOfDiff = (header >> DIFF_WIDTH_SHIFT) & DIFF_WIDTH_MASK;
        if (sizeOfDiff == INTEGER_DIFF_WIDTH_CODE) {
            sizeOfDiff = Integer.BYTES;
        }
        int dataOffset = 1 + sizeOfDiff;
        if (dataOffset > source.length) {
            throw new IllegalArgumentException("LZ4 pickle header is incomplete");
        }
        int dataLength = source.length - dataOffset;
        long resultDiff = sizeOfDiff == 0 ? 0 : peekLittleEndian(source, 1, sizeOfDiff);
        return new PickleHeader(dataOffset, dataLength + resultDiff, resultDiff != 0);
    }

    private static int effectiveSizeOf(int value) {
        if (value < 0 || value > ZLinkStreamWireProtocol.MAX_UNSIGNED_SHORT) {
            return Integer.BYTES;
        }
        if (value > ZLinkStreamWireProtocol.MAX_UNSIGNED_BYTE) {
            return Short.BYTES;
        }
        return Byte.BYTES;
    }

    private static byte encodeHeaderByte(int sizeOfDiff) {
        int encodedSize = sizeOfDiff == Integer.BYTES ? INTEGER_DIFF_WIDTH_CODE : sizeOfDiff;
        return (byte) ((encodedSize & DIFF_WIDTH_MASK) << DIFF_WIDTH_SHIFT);
    }

    private static void pokeLittleEndian(byte[] target, int offset, int value, int size) {
        for (int i = 0; i < size; i++) {
            target[offset + i] =
                    (byte)
                            ((value >>> (i * Byte.SIZE))
                                    & ZLinkStreamWireProtocol.MAX_UNSIGNED_BYTE);
        }
    }

    private static long peekLittleEndian(byte[] source, int offset, int size) {
        long result = 0;
        for (int i = 0; i < size; i++) {
            result |= (long) Byte.toUnsignedInt(source[offset + i]) << (i * Byte.SIZE);
        }
        return result;
    }

    private record PickleHeader(int dataOffset, long resultLength, boolean compressed) {}
}
