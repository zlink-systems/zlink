/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import systems.zlink.httpclient.internal.HttpClientErrors;
import systems.zlink.httpclient.internal.HttpClientText;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;

final class ZLinkHttpRequestBodyEncoder {
    private static final String FORM_CONTENT_TYPE = "application/x-www-form-urlencoded";
    private static final String MULTIPART_CONTENT_TYPE_PREFIX = "multipart/form-data; boundary=";

    private ZLinkHttpRequestBodyEncoder() {}

    record MultipartPart(String name, String filename, String content, String contentType) {}

    record BodyAndHeaders(String body, Map<String, String> headers) {}

    static MultipartPart multipartField(String name, String value) {
        return new MultipartPart(name, "", value, "");
    }

    static MultipartPart multipartFile(
            String name, String filename, String content, String contentType) {
        return new MultipartPart(name, filename, content, contentType);
    }

    static BodyAndHeaders resolve(
            String body,
            Supplier<byte[]> bodyProvider,
            Map<String, String> headers,
            List<Map.Entry<String, String>> form,
            List<MultipartPart> multipart) {
        if (countBodySources(body, bodyProvider, form, multipart) > 1) {
            throw HttpClientErrors.protocol(
                    "HTTP request accepts a single body source: body, body_stream, form, or"
                            + " multipart");
        }

        Map<String, String> resolvedHeaders = new LinkedHashMap<>(headers);
        if (body != null) {
            return new BodyAndHeaders(body, resolvedHeaders);
        }

        if (!form.isEmpty()) {
            resolvedHeaders.put(HttpClientText.Header.CONTENT_TYPE.wire(), FORM_CONTENT_TYPE);
            return new BodyAndHeaders(encodeFormBody(form), resolvedHeaders);
        }

        if (!multipart.isEmpty()) {
            String boundary = HttpClientText.makeMultipartBoundary();
            resolvedHeaders.put(
                    HttpClientText.Header.CONTENT_TYPE.wire(),
                    MULTIPART_CONTENT_TYPE_PREFIX + boundary);
            return new BodyAndHeaders(encodeMultipartBody(multipart, boundary), resolvedHeaders);
        }

        return new BodyAndHeaders(null, resolvedHeaders);
    }

    private static int countBodySources(
            String body,
            Supplier<byte[]> bodyProvider,
            List<Map.Entry<String, String>> form,
            List<MultipartPart> multipart) {
        return (body != null ? 1 : 0)
                + (bodyProvider != null ? 1 : 0)
                + (form.isEmpty() ? 0 : 1)
                + (multipart.isEmpty() ? 0 : 1);
    }

    private static String encodeFormBody(List<Map.Entry<String, String>> form) {
        StringBuilder encoded = new StringBuilder();
        for (Map.Entry<String, String> entry : form) {
            if (encoded.length() > 0) {
                encoded.append('&');
            }
            encoded.append(HttpClientText.percentEncode(entry.getKey()))
                    .append('=')
                    .append(HttpClientText.percentEncode(entry.getValue()));
        }
        return encoded.toString();
    }

    private static String encodeMultipartBody(List<MultipartPart> multipart, String boundary) {
        StringBuilder encoded = new StringBuilder();
        for (MultipartPart part : multipart) {
            encoded.append("--").append(boundary).append("\r\n");
            encoded.append("Content-Disposition: form-data; name=\"")
                    .append(part.name())
                    .append('"');
            if (!part.filename().isEmpty()) {
                encoded.append("; filename=\"").append(part.filename()).append('"');
            }
            encoded.append("\r\n");
            if (!part.contentType().isEmpty()) {
                encoded.append("Content-Type: ").append(part.contentType()).append("\r\n");
            }
            encoded.append("\r\n").append(part.content()).append("\r\n");
        }
        encoded.append("--").append(boundary).append("--\r\n");
        return encoded.toString();
    }
}
