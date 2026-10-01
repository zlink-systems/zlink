/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.zip.GZIPInputStream;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;

/**
 * Wrapper-controlled response decompression mirroring the C++ {@code compression.cpp}: gzip and
 * deflate are decoded, the decoded size is bounded by the configured body limit, and the caller
 * removes the {@code Content-Encoding} header afterwards. {@code java.net.http} does not
 * auto-decompress, so streaming downloads are never transparently decoded and the body limit is
 * enforced against the decoded size. A malformed body raises a decode failure; exceeding the limit
 * raises a rejected response.
 */
public final class ResponseCompression {

    enum Encoding {
        GZIP("gzip"),
        DEFLATE("deflate");

        private final String wire;

        Encoding(String wire) {
            this.wire = wire;
        }

        static Encoding decode(String wire) {
            if (GZIP.wire.equalsIgnoreCase(wire)) {
                return GZIP;
            }
            if (DEFLATE.wire.equalsIgnoreCase(wire)) {
                return DEFLATE;
            }
            return null;
        }
    }

    static final String ACCEPT_ENCODING = Encoding.GZIP.wire + ", " + Encoding.DEFLATE.wire;
    private static final int ZLIB_HEADER_BYTES = 2;
    private static final int COMPRESSION_METHOD_MASK = 0x0f;
    private static final int DEFLATE_METHOD = 8;
    private static final int ZLIB_HEADER_CHECK_DIVISOR = 31;

    private ResponseCompression() {}

    static byte[] decompress(Encoding encoding, byte[] input, long maxBytes) {
        return switch (encoding) {
            case GZIP -> gunzip(input, maxBytes);
            case DEFLATE -> inflateDeflate(input, maxBytes);
        };
    }

    public static byte[] gunzip(byte[] input, long maxBytes) {
        try {
            return decode(new GZIPInputStream(new ByteArrayInputStream(input)), maxBytes);
        } catch (IOException cause) {
            throw malformed(cause);
        }
    }

    public static byte[] inflateDeflate(byte[] input, long maxBytes) {
        // Detect a zlib-wrapped stream (method deflate, header a multiple of 31) vs raw deflate.
        boolean zlibWrapped =
                input.length >= ZLIB_HEADER_BYTES
                        && (input[0] & COMPRESSION_METHOD_MASK) == DEFLATE_METHOD
                        && (((input[0] & 0xff) << Byte.SIZE | (input[1] & 0xff))
                                        % ZLIB_HEADER_CHECK_DIVISOR)
                                == 0;
        try {
            InflaterInputStream stream =
                    zlibWrapped
                            ? new InflaterInputStream(new ByteArrayInputStream(input))
                            : new InflaterInputStream(
                                    new ByteArrayInputStream(input), new Inflater(true));
            return decode(stream, maxBytes);
        } catch (IOException cause) {
            throw malformed(cause);
        }
    }

    private static byte[] decode(InputStream stream, long maxBytes) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        // The limit failure is unchecked, so it escapes the callers' IOException->malformed
        // mapping.
        BoundedRead.copy(
                stream,
                maxBytes,
                ResponseBodyReader::tooLarge,
                (buffer, length) -> output.write(buffer, 0, length));
        return output.toByteArray();
    }

    private static ZLinkFrameworkException malformed(IOException cause) {
        return HttpClientErrors.protocol("HTTP response compressed body is malformed", cause);
    }
}
