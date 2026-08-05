/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.internal.HttpClientText;

final class ZLinkHttpRequestBodyEncoder {
    private ZLinkHttpRequestBodyEncoder() {
    }

    record MultipartPart(String name, String filename, String content, String contentType) {
    }

    record BodyAndHeaders(String body, Map<String, String> headers) {
    }

    static MultipartPart multipartField(String name, String value) {
        return new MultipartPart(name, "", value, "");
    }

    static MultipartPart multipartFile(
        String name,
        String filename,
        String content,
        String contentType) {
        return new MultipartPart(name, filename, content, contentType);
    }

    static BodyAndHeaders resolve(
        String body,
        Supplier<byte[]> bodyProvider,
        Map<String, String> headers,
        List<Map.Entry<String, String>> form,
        List<MultipartPart> multipart) {
        if (countBodySources(body, bodyProvider, form, multipart) > 1) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                "HTTP request accepts a single body source: body, body_stream, form, or multipart");
        }

        Map<String, String> resolvedHeaders = new LinkedHashMap<>(headers);
        if (body != null) {
            return new BodyAndHeaders(body, resolvedHeaders);
        }

        if (!form.isEmpty()) {
            resolvedHeaders.put("content-type", "application/x-www-form-urlencoded");
            return new BodyAndHeaders(encodeFormBody(form), resolvedHeaders);
        }

        if (!multipart.isEmpty()) {
            String boundary = HttpClientText.makeMultipartBoundary();
            resolvedHeaders.put("content-type", "multipart/form-data; boundary=" + boundary);
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
            encoded.append("Content-Disposition: form-data; name=\"").append(part.name()).append('"');
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
