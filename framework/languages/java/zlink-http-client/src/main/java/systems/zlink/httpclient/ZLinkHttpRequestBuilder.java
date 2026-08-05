/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;
import java.time.Duration;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.internal.HttpClientText;
import systems.zlink.httpclient.internal.HttpRequestSpec;

/**
 * Fluent builder for a single request. Mirrors the C++ {@code request_builder_t}. Submission returns
 * a {@link CompletionStage}; no thread is parked while the request is in flight.
 */
public final class ZLinkHttpRequestBuilder {

    private static final ObjectMapper MAPPER = JsonMapper.builder()
        .findAndAddModules()
        .build();

    private final RequestClientLease clientLease;
    private final ZLinkHttpMethod method;
    private final String path;
    private String body;
    private Supplier<byte[]> bodyProvider;
    private final Map<String, String> headers = new LinkedHashMap<>();
    private Duration timeout;
    private final List<Map.Entry<String, String>> query = new ArrayList<>();
    private final List<Map.Entry<String, String>> form = new ArrayList<>();
    private final List<ZLinkHttpRequestBodyEncoder.MultipartPart> multipart = new ArrayList<>();

    ZLinkHttpRequestBuilder(ZLinkHttpClient client, ZLinkHttpMethod method, String path) {
        this.clientLease = RequestClientLease.shared(client);
        this.method = method;
        this.path = path;
        validatePath(path);
    }

    // One-shot constructor: the client is built lazily at the terminal operation and closed
    // afterwards, so any pre-submit validation failure cannot leak an eagerly-built client.
    ZLinkHttpRequestBuilder(ZLinkHttpClientBuilder clientFactory, ZLinkHttpMethod method, String path) {
        this.clientLease = RequestClientLease.oneShot(clientFactory);
        this.method = method;
        this.path = path;
        validatePath(path);
    }

    private static void validatePath(String path) {
        if (path.isEmpty() || path.charAt(0) != '/') {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP request path must start with /");
        }
    }

    public ZLinkHttpRequestBuilder header(String name, String value) {
        HttpClientText.requireNonBlank(name, "HTTP request header name is required");
        headers.put(name.toLowerCase(Locale.ROOT), value);
        return this;
    }

    public ZLinkHttpRequestBuilder query(String name, String value) {
        HttpClientText.requireNonBlank(name, "HTTP request query name is required");
        query.add(Map.entry(name, value));
        return this;
    }

    public ZLinkHttpRequestBuilder timeout(Duration value) {
        this.timeout = HttpClientText.normalizeTimeout(value);
        return this;
    }

    /** Sets a typed JSON body. Serialized with Jackson; sets {@code content-type}. */
    public ZLinkHttpRequestBuilder body(Object value) {
        try {
            this.body = MAPPER.writeValueAsString(value);
        } catch (Exception cause) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP request body could not be serialized", cause);
        }
        headers.putIfAbsent("content-type", "application/json");
        return this;
    }

    /** Sets a raw body with an explicit content type. */
    public ZLinkHttpRequestBuilder body(String content, String contentType) {
        if (content == null) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP request raw body content is required");
        }
        HttpClientText.requireNonBlank(contentType, "HTTP request body content type is required");
        this.body = content;
        headers.put("content-type", contentType);
        return this;
    }

    /**
     * Streams the request body chunk by chunk with chunked transfer-encoding; the provider returns
     * {@code null} when the body is complete. Streamed requests are excluded from retry.
     */
    public ZLinkHttpRequestBuilder bodyStream(Supplier<byte[]> provider, String contentType) {
        if (provider == null) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP request body stream provider is required");
        }
        HttpClientText.requireNonBlank(contentType, "HTTP request body content type is required");
        this.bodyProvider = provider;
        headers.put("content-type", contentType);
        return this;
    }

    public ZLinkHttpRequestBuilder form(String name, String value) {
        HttpClientText.requireNonBlank(name, "HTTP request form field name is required");
        form.add(Map.entry(name, value));
        return this;
    }

    public ZLinkHttpRequestBuilder multipart(String name, String value) {
        HttpClientText.requireNonBlank(name, "HTTP request multipart field name is required");
        multipart.add(ZLinkHttpRequestBodyEncoder.multipartField(name, value));
        return this;
    }

    public ZLinkHttpRequestBuilder multipartFile(String name, String filename, String content, String contentType) {
        HttpClientText.requireNonBlank(name, "HTTP request multipart field name is required");
        HttpClientText.requireNonBlank(filename, "HTTP request multipart filename is required");
        HttpClientText.requireNonBlank(contentType, "HTTP request multipart content type is required");
        multipart.add(ZLinkHttpRequestBodyEncoder.multipartFile(name, filename, content, contentType));
        return this;
    }

    /** Submits the request and returns the raw response. */
    public CompletionStage<RawHttpResponse> submitRaw() {
        return execute(null);
    }

    /**
     * Streams the response body to {@code sink} chunk by chunk instead of buffering it; the returned
     * response carries status and headers with an empty body (no decompression of chunks).
     */
    public CompletionStage<RawHttpResponse> download(Consumer<byte[]> sink) {
        if (sink == null) {
            throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP request download sink is required");
        }
        return execute(sink);
    }

    private CompletionStage<RawHttpResponse> execute(Consumer<byte[]> sink) {
        var spec = makeRequest(sink);
        ZLinkHttpClient resolved = clientLease.acquire();
        try {
            return resolved.runtime().executeAsync(spec)
                .thenApply(result -> new RawHttpResponse(result.status(), result.headers(), result.body()))
                .whenComplete((result, error) -> clientLease.release());
        } catch (RuntimeException error) {
            clientLease.release();
            throw error;
        }
    }

    /** Submits the request and decodes the JSON body to {@code type}. */
    public <T> CompletionStage<HttpResponse<T>> submit(Class<T> type) {
        return decode(submitRaw(), type);
    }

    /** Submits the request and returns only the decoded JSON body. */
    public <T> CompletionStage<T> fetch(Class<T> type) {
        return submit(type).thenApply(HttpResponse::body);
    }

    public <T> void submit(Class<T> type, ZLinkHttpCallback<T> callback) {
        java.util.Objects.requireNonNull(callback, "callback");
        submit(type).whenComplete((response, error) ->
            callback.complete(error, error == null ? response : null));
    }

    private static <T> CompletionStage<HttpResponse<T>> decode(
        CompletionStage<RawHttpResponse> operation,
        Class<T> type) {
        return operation.thenApply(raw -> {
            if (raw.status() >= 400) {
                throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "HTTP request failed with status " + raw.status());
            }
            if (raw.body().isEmpty()) {
                return new HttpResponse<>(raw.status(), raw.headers(), null, raw.body());
            }
            try {
                T body = MAPPER.readValue(raw.body(), type);
                return new HttpResponse<>(raw.status(), raw.headers(), body, raw.body());
            } catch (Exception cause) {
                throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "HTTP response body decode failed", cause);
            }
        });
    }

    private HttpRequestSpec makeRequest(Consumer<byte[]> sink) {
        ZLinkHttpRequestBodyEncoder.BodyAndHeaders resolved = ZLinkHttpRequestBodyEncoder.resolve(
            body,
            bodyProvider,
            headers,
            form,
            multipart);
        return new HttpRequestSpec(
            method,
            ZLinkHttpTargetBuilder.resolve(path, query),
            resolved.body(),
            bodyProvider,
            resolved.headers(),
            timeout,
            sink);
    }

    private static final class RequestClientLease {
        private final ZLinkHttpClientBuilder factory;
        private final boolean owned;
        private ZLinkHttpClient client;
        private boolean acquired;

        private RequestClientLease(
            ZLinkHttpClient client,
            ZLinkHttpClientBuilder factory,
            boolean owned) {
            this.client = client;
            this.factory = factory;
            this.owned = owned;
        }

        static RequestClientLease shared(ZLinkHttpClient client) {
            return new RequestClientLease(client, null, false);
        }

        static RequestClientLease oneShot(ZLinkHttpClientBuilder factory) {
            return new RequestClientLease(null, factory, true);
        }

        synchronized ZLinkHttpClient acquire() {
            if (owned && acquired) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "A one-shot HTTP request can only be submitted once");
            }
            acquired = true;
            if (client == null) {
                client = factory.build();
            }
            return client;
        }

        synchronized void release() {
            if (!owned || client == null) {
                return;
            }
            try {
                client.close();
            } catch (RuntimeException ignored) {
                // Cleanup must not replace the request result.
            } finally {
                client = null;
            }
        }
    }
}
