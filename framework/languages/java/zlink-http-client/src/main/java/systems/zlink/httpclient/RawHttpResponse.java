/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.util.Map;

/**
 * Raw HTTP response with status, headers, and the buffered body as a string. Header lookup keys are
 * lowercase. Mirrors the C++ {@code raw_http_response_t}.
 */
public record RawHttpResponse(int status, Map<String, String> headers, String body) {
}
