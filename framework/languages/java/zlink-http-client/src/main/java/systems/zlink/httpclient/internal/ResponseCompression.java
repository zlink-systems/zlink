/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.zip.GZIPInputStream;
import java.util.zip.Inflater;
import java.util.zip.InflaterInputStream;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Wrapper-controlled response decompression mirroring the C++ {@code compression.cpp}: gzip and
 * deflate are decoded, the decoded size is bounded by the configured body limit, and the caller
 * removes the {@code Content-Encoding} header afterwards. {@code java.net.http} does not
 * auto-decompress, so streaming downloads are never transparently decoded and the body limit is
 * enforced against the decoded size. A malformed body raises a decode failure; exceeding the limit
 * raises a request failure.
 */
public final class ResponseCompression {

    private ResponseCompression() {
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
        boolean zlibWrapped = input.length >= 2
            && (input[0] & 0x0f) == 8
            && (((input[0] & 0xff) << 8 | (input[1] & 0xff)) % 31) == 0;
        try {
            InflaterInputStream stream = zlibWrapped
                ? new InflaterInputStream(new ByteArrayInputStream(input))
                : new InflaterInputStream(new ByteArrayInputStream(input), new Inflater(true));
            return decode(stream, maxBytes);
        } catch (IOException cause) {
            throw malformed(cause);
        }
    }

    private static byte[] decode(java.io.InputStream stream, long maxBytes) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        // The limit failure is unchecked, so it escapes the callers' IOException->malformed
        // mapping and keeps its REQUEST_FAILED kind.
        BoundedRead.copy(stream, maxBytes,
            () -> new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "HTTP response compressed body exceeds maxResponseBodySize"),
            (buffer, length) -> output.write(buffer, 0, length));
        return output.toByteArray();
    }

    private static ZLinkFrameworkException malformed(IOException cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP response compressed body is malformed", cause);
    }
}
