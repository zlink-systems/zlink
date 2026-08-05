/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.UncheckedIOException;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.function.Consumer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

/**
 * Reads and decodes HTTP response bodies for the wrapper: buffered read with the configured size
 * limit, streaming delivery to a sink, header collection, and wrapper-controlled gzip/deflate
 * decompression. Separated from {@link RequestPerformer} so response decoding is independent of the
 * request/redirect flow. Mirrors the dotnet/node {@code ResponseBodyReader}.
 */
final class ResponseBodyReader {

    /** A decoded body together with the headers after any {@code Content-Encoding} removal. */
    record DecodedBody(byte[] body, Map<String, String> headers) {
    }

    private final HttpClientOptions options;

    ResponseBodyReader(HttpClientOptions options) {
        this.options = options;
    }

    void streamToSink(InputStream stream, Consumer<byte[]> sink) {
        try {
            BoundedRead.copy(stream, options.maxResponseBodySize(), ResponseBodyReader::tooLarge,
                (buffer, length) -> {
                    byte[] chunk = new byte[length];
                    System.arraycopy(buffer, 0, chunk, 0, length);
                    sink.accept(chunk);
                });
        } catch (IOException cause) {
            // Surface as an unchecked IOException so RetryPolicy can classify a body-read transport
            // failure (e.g. connection reset) as retriable, rather than masking it.
            throw new UncheckedIOException(cause);
        }
    }

    byte[] readBuffered(InputStream stream) {
        try {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            BoundedRead.copy(stream, options.maxResponseBodySize(), ResponseBodyReader::tooLarge,
                (buffer, length) -> output.write(buffer, 0, length));
            return output.toByteArray();
        } catch (IOException cause) {
            // Surface as an unchecked IOException so RetryPolicy can classify a body-read transport
            // failure (e.g. connection reset) as retriable, rather than masking it.
            throw new UncheckedIOException(cause);
        }
    }

    private static ZLinkFrameworkException tooLarge() {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "HTTP response exceeded the maximum body size");
    }

    DecodedBody decompress(byte[] bytes, Map<String, String> headers) {
        String encoding = findHeader(headers, "content-encoding");
        // An empty body (HEAD / 204 / 304) carries no payload to decode even with Content-Encoding.
        if (encoding == null || bytes.length == 0) {
            return new DecodedBody(bytes, headers);
        }
        if (encoding.equalsIgnoreCase("gzip")) {
            return new DecodedBody(
                ResponseCompression.gunzip(bytes, options.maxResponseBodySize()),
                stripEncodingHeaders(headers));
        }
        if (encoding.equalsIgnoreCase("deflate")) {
            return new DecodedBody(
                ResponseCompression.inflateDeflate(bytes, options.maxResponseBodySize()),
                stripEncodingHeaders(headers));
        }
        return new DecodedBody(bytes, headers);
    }

    static Map<String, String> collectHeaders(Map<String, List<String>> headers) {
        Map<String, String> result = new LinkedHashMap<>();
        for (Map.Entry<String, List<String>> entry : headers.entrySet()) {
            result.put(entry.getKey().toLowerCase(Locale.ROOT), String.join(", ", entry.getValue()));
        }
        return result;
    }

    static String findHeader(Map<String, String> headers, String name) {
        return headers.get(name.toLowerCase(Locale.ROOT));
    }

    // After decoding, drop Content-Encoding and the now-stale Content-Length (it described the
    // compressed body, not the decoded one).
    private static Map<String, String> stripEncodingHeaders(Map<String, String> headers) {
        Map<String, String> copy = new LinkedHashMap<>();
        for (Map.Entry<String, String> entry : headers.entrySet()) {
            String name = entry.getKey().toLowerCase(Locale.ROOT);
            if (!name.equals("content-encoding") && !name.equals("content-length")) {
                copy.put(entry.getKey(), entry.getValue());
            }
        }
        return copy;
    }

    String decodeText(byte[] bytes) {
        return new String(bytes, StandardCharsets.UTF_8);
    }
}
