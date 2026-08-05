/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.time.Duration;
import java.util.Map;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.httpclient.ZLinkHttpMethod;

/**
 * Internal description of a single request as assembled by the request builder. Mirrors the C++
 * {@code http_request_t}. {@code target} is the path (starting with {@code /}) with percent-encoded
 * query already appended.
 */
public record HttpRequestSpec(
    ZLinkHttpMethod method,
    String target,
    String body,
    Supplier<byte[]> bodyProvider,
    Map<String, String> headers,
    Duration timeout,
    Consumer<byte[]> sink) {

    public boolean isStreaming() {
        return sink != null || bodyProvider != null;
    }
}
