/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Map;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.internal.HttpClientOptions;
import systems.zlink.httpclient.internal.HttpClientRuntime;
import systems.zlink.httpclient.internal.HttpClientText;

/** Fluent builder for {@link ZLinkHttpClient}. Mirrors the C++ {@code client_builder_t}. */
public final class ZLinkHttpClientBuilder {

    private String baseUrl = "";
    private Duration timeout = Duration.ofMillis(3000);
    private long maxResponseBodySize = 16L * 1024 * 1024;
    private final Map<String, String> headers = new LinkedHashMap<>();
    private String trustCertificateFile;
    private HttpClientOptions.ClientCertificate clientCertificate;
    private int followRedirects;
    private int retryAttempts;
    private boolean cookies;
    private String proxy;
    private String proxyAuthorization;
    private boolean compression;

    public ZLinkHttpClientBuilder baseUrl(String value) {
        HttpClientText.requireNonBlank(value, "HTTP client base_url is required");
        this.baseUrl = value;
        return this;
    }

    public ZLinkHttpClientBuilder timeout(Duration value) {
        this.timeout = HttpClientText.normalizeTimeout(value);
        return this;
    }

    public ZLinkHttpClientBuilder defaultHeader(String name, String value) {
        HttpClientText.requireNonBlank(name, "HTTP client default header name is required");
        this.headers.put(name.toLowerCase(java.util.Locale.ROOT), value);
        return this;
    }

    public ZLinkHttpClientBuilder basicAuth(String user, String password) {
        HttpClientText.requireNonBlank(user, "HTTP client basic auth user is required");
        this.headers.put("authorization", HttpClientText.basicAuthorization(user, password));
        return this;
    }

    public ZLinkHttpClientBuilder bearerToken(String token) {
        HttpClientText.requireNonBlank(token, "HTTP client bearer token is required");
        this.headers.put("authorization", "Bearer " + token);
        return this;
    }

    public ZLinkHttpClientBuilder maxResponseBodySize(long bytes) {
        if (bytes <= 0) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client max response body size must be greater than zero");
        }
        this.maxResponseBodySize = bytes;
        return this;
    }

    public ZLinkHttpClientBuilder trustCertificateFile(String path) {
        HttpClientText.requireNonBlank(path, "HTTP client trust certificate file is required");
        this.trustCertificateFile = path;
        return this;
    }

    public ZLinkHttpClientBuilder clientCertificateFile(String certificatePath, String keyPath) {
        HttpClientText.requireNonBlank(certificatePath, "HTTP client certificate file is required");
        HttpClientText.requireNonBlank(keyPath, "HTTP client certificate key file is required");
        this.clientCertificate = new HttpClientOptions.ClientCertificate(certificatePath, keyPath);
        return this;
    }

    public ZLinkHttpClientBuilder followRedirects() {
        return followRedirects(5);
    }

    public ZLinkHttpClientBuilder followRedirects(int maxRedirects) {
        if (maxRedirects <= 0) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client follow_redirects must be greater than zero");
        }
        this.followRedirects = maxRedirects;
        return this;
    }

    public ZLinkHttpClientBuilder retry(int attempts) {
        if (attempts <= 0) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client retry attempts must be greater than zero");
        }
        this.retryAttempts = attempts;
        return this;
    }

    public ZLinkHttpClientBuilder cookies() {
        this.cookies = true;
        return this;
    }

    public ZLinkHttpClientBuilder proxy(String url) {
        HttpClientText.requireNonBlank(url, "HTTP client proxy url is required");
        if (!url.startsWith("http://")) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client proxy url must start with http://");
        }
        this.proxy = url;
        return this;
    }

    public ZLinkHttpClientBuilder proxyBasicAuth(String user, String password) {
        HttpClientText.requireNonBlank(user, "HTTP client proxy auth user is required");
        this.proxyAuthorization = HttpClientText.basicAuthorization(user, password);
        return this;
    }

    public ZLinkHttpClientBuilder compression() {
        this.compression = true;
        return this;
    }

    /** Builds an immutable client. Validation mirrors the C++ {@code build()}. */
    public ZLinkHttpClient build() {
        HttpClientText.requireNonBlank(baseUrl, "HTTP client base_url is required");
        HttpClientText.requirePositiveTimeout(timeout);
        String lower = baseUrl.toLowerCase(java.util.Locale.ROOT);
        if (!lower.startsWith("http://") && !lower.startsWith("https://")) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP client base_url must start with http:// or https://");
        }

        HttpClientOptions options = new HttpClientOptions(
            baseUrl,
            timeout,
            maxResponseBodySize,
            Map.copyOf(headers),
            trustCertificateFile,
            clientCertificate,
            followRedirects,
            retryAttempts,
            cookies,
            proxy,
            proxyAuthorization,
            compression);
        return new ZLinkHttpClient(new HttpClientRuntime(options));
    }

    public ZLinkHttpServerClient buildServer(ZLinkHttpExecutionTurn executionTurn) {
        return new ZLinkHttpServerClient(
            build(), executionTurn, Throwable::printStackTrace);
    }

    public ZLinkHttpRequestBuilder get(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.GET, path);
    }

    public ZLinkHttpRequestBuilder post(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.POST, path);
    }

    public ZLinkHttpRequestBuilder put(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.PUT, path);
    }

    public ZLinkHttpRequestBuilder delete(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.DELETE, path);
    }

    public ZLinkHttpRequestBuilder patch(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.PATCH, path);
    }

    public ZLinkHttpRequestBuilder head(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.HEAD, path);
    }

    public ZLinkHttpRequestBuilder options(String path) {
        return new ZLinkHttpRequestBuilder(this, ZLinkHttpMethod.OPTIONS, path);
    }
}
