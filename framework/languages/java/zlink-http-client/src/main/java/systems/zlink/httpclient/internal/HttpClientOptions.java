/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.time.Duration;
import java.util.Map;

/**
 * Immutable snapshot of the client configuration produced by the builder. Mirrors the C++
 * {@code http_client_options_t}. Transport gaps (cookie jar, redirect loop, compression, retry) are
 * implemented by the wrapper; only matching semantics are delegated to {@code java.net.http}.
 */
public record HttpClientOptions(
    String baseUrl,
    Duration timeout,
    long maxResponseBodySize,
    Map<String, String> headers,
    String trustCertificateFile,
    ClientCertificate clientCertificate,
    int followRedirects,
    int retryAttempts,
    boolean cookies,
    String proxy,
    String proxyAuthorization,
    boolean compression) {

    /** mTLS client certificate paths. */
    public record ClientCertificate(String certificatePath, String keyPath) {
    }
}
