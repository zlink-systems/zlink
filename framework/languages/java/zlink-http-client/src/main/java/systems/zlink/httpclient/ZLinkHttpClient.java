/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import systems.zlink.httpclient.internal.HttpClientRuntime;

/**
 * ZLink-style fluent HTTP client. Wraps {@code java.net.http.HttpClient} behind a builder so
 * transport types never leak into application code. A general HTTP client; the typed-JSON path
 * ({@code body(dto)} / {@code submit(Type)}) is a convenience layer on top.
 * Mirrors the C++ {@code zlink::http_client::client_t}.
 */
public final class ZLinkHttpClient implements AutoCloseable {

    private final HttpClientRuntime runtime;

    ZLinkHttpClient(HttpClientRuntime runtime) {
        this.runtime = runtime;
    }

    HttpClientRuntime runtime() {
        return runtime;
    }

    /** Starts a new client builder. */
    public static ZLinkHttpClientBuilder create() {
        return new ZLinkHttpClientBuilder();
    }

    /** Starts a new client builder with a base URL. */
    public static ZLinkHttpClientBuilder create(String baseUrl) {
        return new ZLinkHttpClientBuilder().baseUrl(baseUrl);
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

    @Override
    public void close() {
        runtime.close();
    }
}
