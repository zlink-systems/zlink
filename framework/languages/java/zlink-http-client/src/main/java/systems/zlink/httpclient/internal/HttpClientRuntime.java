/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.net.http.HttpClient;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import systems.zlink.httpclient.internal.RequestPerformer.RawResult;

/**
 * Wires the {@link HttpClient}, request performer, and retry policy. Client/TLS construction lives in
 * {@link JavaHttpClientFactory} and the retry loop in {@link RetryPolicy}. Submission is the caller's
 * native {@code CompletableFuture}; no thread is parked while the request is in flight. Mirrors the
 * C++ {@code http_client_runtime.cpp}.
 */
public final class HttpClientRuntime implements AutoCloseable {

    private final HttpClientOptions options;
    private final HttpClient httpClient;
    private final ExecutorService executor;
    private final ScheduledExecutorService timeoutScheduler;
    private final CookieJar cookieJar = new CookieJar();
    private final RequestPerformer performer;
    private final RetryPolicy retryPolicy;

    public HttpClientRuntime(HttpClientOptions options) {
        this.options = options;
        this.executor = Executors.newCachedThreadPool(runnable -> {
            Thread thread = new Thread(runnable, "zlink-http-client");
            thread.setDaemon(true);
            return thread;
        });
        this.timeoutScheduler = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "zlink-http-client-timeout");
            thread.setDaemon(true);
            return thread;
        });
        this.httpClient = JavaHttpClientFactory.create(options, executor);
        this.performer = new RequestPerformer(options, cookieJar, httpClient, executor, timeoutScheduler);
        this.retryPolicy = new RetryPolicy(executor);
    }

    /**
     * Executes the request, applying the retry policy. Streaming requests are never retried because
     * they cannot be rewound. The submission API is the caller's {@code CompletionStage}.
     */
    public CompletionStage<RawResult> executeAsync(HttpRequestSpec spec) {
        int maxRetries = spec.isStreaming() ? 0 : options.retryAttempts();
        return retryPolicy.execute(maxRetries, () -> performer.performAsync(spec));
    }

    @Override
    public void close() {
        httpClient.close();
        executor.shutdown();
        timeoutScheduler.shutdownNow();
    }
}
