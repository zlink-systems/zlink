/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.InputStream;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpRequest.BodyPublisher;
import java.net.http.HttpRequest.BodyPublishers;
import java.net.http.HttpResponse.BodyHandlers;
import java.io.IOException;
import java.time.Duration;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.ZLinkHttpMethod;

/**
 * Performs a single logical request, driving the wrapper-owned redirect loop, cookie jar
 * integration, and same-origin {@code Authorization} scrubbing. Redirect/URL rules live in
 * {@link RedirectPolicy} and response decoding in {@link ResponseBodyReader}. Mirrors the C++
 * {@code request_performer.cpp}. The redirect loop is an async {@link CompletionStage} chain so no
 * thread is parked between hops; the blocking body read is offloaded to an executor.
 */
public final class RequestPerformer {

    /** Result of a performed request before typed decoding. */
    public record RawResult(int status, Map<String, String> headers, String body) {
    }

    private final HttpClientOptions options;
    private final CookieJar cookieJar;
    private final HttpClient httpClient;
    private final Executor executor;
    private final ScheduledExecutorService timeoutScheduler;
    private final ResponseBodyReader bodyReader;

    public RequestPerformer(
        HttpClientOptions options,
        CookieJar cookieJar,
        HttpClient httpClient,
        Executor executor,
        ScheduledExecutorService timeoutScheduler) {
        this.options = options;
        this.cookieJar = cookieJar;
        this.httpClient = httpClient;
        this.executor = executor;
        this.timeoutScheduler = timeoutScheduler;
        this.bodyReader = new ResponseBodyReader(options);
    }

    public CompletionStage<RawResult> performAsync(HttpRequestSpec spec) {
        URI base = URI.create(options.baseUrl());
        String origin = RedirectPolicy.originOf(base);
        URI current = URI.create(origin + RedirectPolicy.makeTarget(base.getRawPath(), spec.target()));
        Duration timeout = spec.timeout() != null ? spec.timeout() : options.timeout();
        long deadlineNanos = System.nanoTime() + timeout.toNanos();
        // A streamed body provider cannot be rewound, so it is dropped once a redirect is followed.
        return hop(spec, current, origin, spec.method(), spec.body(), spec.bodyProvider(),
            options.followRedirects(), deadlineNanos);
    }

    private CompletionStage<RawResult> hop(
        HttpRequestSpec spec, URI current, String origin, ZLinkHttpMethod method, String body,
        Supplier<byte[]> bodyProvider, int redirectsLeft, long deadlineNanos) {
        Duration remaining = remaining(deadlineNanos);
        if (remaining == null) {
            return CompletableFuture.failedFuture(
                new java.net.http.HttpTimeoutException("HTTP request attempt timed out"));
        }
        boolean keepAuthorization = origin.equals(RedirectPolicy.originOf(current));
        HttpRequest request = buildRequest(
            spec, current, method, body, bodyProvider, keepAuthorization, remaining);
        return httpClient
            .sendAsync(request, BodyHandlers.ofInputStream())
            .thenCompose(response -> handle(spec, current, origin, method, body, bodyProvider,
                redirectsLeft, deadlineNanos, response));
    }

    private CompletionStage<RawResult> handle(
        HttpRequestSpec spec,
        URI current,
        String origin,
        ZLinkHttpMethod method,
        String body,
        Supplier<byte[]> bodyProvider,
        int redirectsLeft,
        long deadlineNanos,
        java.net.http.HttpResponse<InputStream> response) {
        int status = response.statusCode();
        if (options.cookies()) {
            for (String setCookie : response.headers().allValues("set-cookie")) {
                cookieJar.store(current.getHost(), setCookie);
            }
        }

        String location = response.headers().firstValue("location").orElse(null);
        if (options.followRedirects() > 0 && RedirectPolicy.isRedirect(status) && location != null && !location.isEmpty()) {
            closeQuietly(response.body());
            if (redirectsLeft == 0) {
                throw new ZLinkFrameworkException(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "HTTP request exceeded the redirect limit");
            }
            RedirectPolicy.Rewrite rewrite = RedirectPolicy.rewriteMethodAndBody(status, method, body);
            return hop(spec, RedirectPolicy.resolveLocation(current, location), origin,
                rewrite.method(), rewrite.body(), null, redirectsLeft - 1, deadlineNanos);
        }

        Map<String, String> headers = ResponseBodyReader.collectHeaders(response.headers().map());
        Duration remaining = remaining(deadlineNanos);
        if (remaining == null) {
            closeQuietly(response.body());
            return CompletableFuture.failedFuture(
                new java.net.http.HttpTimeoutException("HTTP request attempt timed out"));
        }
        long bodyReadMillis = Math.max(1L, remaining.toMillis());
        InputStream bodyStream = response.body();
        if (spec.sink() != null) {
            return readWithDeadline(bodyStream, bodyReadMillis, () -> {
                bodyReader.streamToSink(bodyStream, spec.sink());
                return new RawResult(status, headers, "");
            });
        }
        return readWithDeadline(bodyStream, bodyReadMillis, () -> {
            byte[] bytes = bodyReader.readBuffered(bodyStream);
            ResponseBodyReader.DecodedBody decoded = options.compression()
                ? bodyReader.decompress(bytes, headers)
                : new ResponseBodyReader.DecodedBody(bytes, headers);
            return new RawResult(status, decoded.headers(), bodyReader.decodeText(decoded.body()));
        });
    }

    /**
     * Runs the offloaded body read with a deadline. {@code ofInputStream} completes {@code sendAsync}
     * after headers, so the HttpRequest timeout no longer covers body reading. Closing the stream at
     * the deadline unblocks a stalled read — freeing the worker thread and the connection rather than
     * leaving them hung — and the deadline is cancelled once the read finishes.
     */
    private CompletableFuture<RawResult> readWithDeadline(InputStream stream, long timeoutMillis, Supplier<RawResult> read) {
        ScheduledFuture<?> deadline =
            timeoutScheduler.schedule(() -> closeQuietly(stream), timeoutMillis, TimeUnit.MILLISECONDS);
        return CompletableFuture.supplyAsync(() -> {
            try {
                return read.get();
            } finally {
                deadline.cancel(false);
            }
        }, executor);
    }

    private HttpRequest buildRequest(
        HttpRequestSpec spec, URI current, ZLinkHttpMethod method, String body,
        Supplier<byte[]> bodyProvider, boolean keepAuthorization, Duration timeout) {
        BodyPublisher publisher;
        if (bodyProvider != null) {
            publisher = BodyPublishers.ofInputStream(() -> new ProviderInputStream(bodyProvider));
        } else if (body != null) {
            publisher = BodyPublishers.ofString(body);
        } else {
            publisher = BodyPublishers.noBody();
        }

        Map<String, String> headers = new LinkedHashMap<>();
        headers.put("user-agent", HttpClientVersion.USER_AGENT);
        headers.put("accept", "application/json");
        if (options.compression()) {
            headers.put("accept-encoding", "gzip, deflate");
        }
        mergeHeaders(headers, options.headers(), keepAuthorization);
        mergeHeaders(headers, spec.headers(), keepAuthorization);
        if (body == null && bodyProvider == null) {
            // A body source dropped by a redirect must not leave stale content-type metadata behind.
            headers.remove("content-type");
        }
        if (options.cookies()) {
            String cookieHeader = cookieJar.headerFor(
                current.getHost(), RedirectPolicy.pathOf(current), "https".equals(current.getScheme()));
            if (!cookieHeader.isEmpty()) {
                headers.put("cookie", cookieHeader);
            }
        }

        HttpRequest.Builder builder = HttpRequest.newBuilder(current)
            .version(HttpClient.Version.HTTP_1_1)
            .timeout(timeout)
            .method(method.name(), publisher);
        headers.forEach(builder::header);
        return builder.build();
    }

    private static Duration remaining(long deadlineNanos) {
        long nanos = deadlineNanos - System.nanoTime();
        return nanos <= 0 ? null : Duration.ofNanos(nanos);
    }

    private static void mergeHeaders(Map<String, String> target, Map<String, String> headers, boolean keepAuthorization) {
        for (Map.Entry<String, String> entry : headers.entrySet()) {
            String name = entry.getKey().toLowerCase(Locale.ROOT);
            if (!keepAuthorization && name.equals("authorization")) {
                continue;
            }
            target.put(name, entry.getValue());
        }
    }

    private static void closeQuietly(InputStream stream) {
        try {
            stream.close();
        } catch (IOException ignored) {
            // discard
        }
    }
}
