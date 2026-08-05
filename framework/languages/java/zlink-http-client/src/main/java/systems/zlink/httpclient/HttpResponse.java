/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.Map;

/**
 * Typed HTTP response. {@code body} is the JSON-decoded payload; {@code rawBody} keeps the original
 * response text. Mirrors the C++ {@code http_response_t<T>}.
 *
 * @param <T> the decoded body type
 */
public record HttpResponse<T>(int status, Map<String, String> headers, T body, String rawBody) {
}
