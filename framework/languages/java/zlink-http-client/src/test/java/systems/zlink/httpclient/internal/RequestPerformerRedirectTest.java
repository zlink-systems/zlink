/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.httpclient.ZLinkHttpMethod;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.net.Authenticator;
import java.net.CookieHandler;
import java.net.ProxySelector;
import java.net.URI;
import java.net.http.HttpClient;
import java.net.http.HttpHeaders;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.net.http.HttpTimeoutException;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.SSLSession;

final class RequestPerformerRedirectTest {
    @Test
    void consumesAndClosesIntermediateRedirectBodyBeforeFollowing() {
        var consumed = new AtomicBoolean();
        var closed = new AtomicBoolean();
        var intermediate =
                new java.io.InputStream() {
                    private final ByteArrayInputStream source =
                            new ByteArrayInputStream("intermediate".getBytes());

                    @Override
                    public int read() {
                        int value = source.read();
                        if (value < 0) consumed.set(true);
                        return value;
                    }

                    @Override
                    public int read(byte[] bytes, int offset, int length) {
                        int count = source.read(bytes, offset, length);
                        if (count < 0) consumed.set(true);
                        return count;
                    }

                    @Override
                    public void close() {
                        closed.set(true);
                    }
                };
        HttpClient client = new RedirectHttpClient(intermediate);
        ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        try {
            var options =
                    new HttpClientOptions(
                            "http://example.test",
                            Duration.ofSeconds(5),
                            1024,
                            Map.of(),
                            null,
                            null,
                            2,
                            0,
                            false,
                            null,
                            null,
                            false);
            var performer =
                    new RequestPerformer(
                            options, new CookieJar(), client, Runnable::run, scheduler);
            var result =
                    performer
                            .performAsync(
                                    new HttpRequestSpec(
                                            ZLinkHttpMethod.GET,
                                            "/start",
                                            null,
                                            null,
                                            Map.of(),
                                            null,
                                            null))
                            .toCompletableFuture()
                            .join();
            assertEquals(200, result.status());
            assertTrue(consumed.get());
            assertTrue(closed.get());
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void deadlineDuringIntermediateBodyConsumptionClosesOnceAndTimesOut() throws Exception {
        CountDownLatch reading = new CountDownLatch(1);
        CountDownLatch closed = new CountDownLatch(1);
        AtomicInteger closeCount = new AtomicInteger();
        var intermediate =
                new java.io.InputStream() {
                    @Override
                    public int read() throws IOException {
                        byte[] one = new byte[1];
                        return read(one, 0, 1);
                    }

                    @Override
                    public int read(byte[] bytes, int offset, int length) throws IOException {
                        reading.countDown();
                        try {
                            if (!closed.await(2, TimeUnit.SECONDS)) {
                                throw new IOException("body was not closed");
                            }
                        } catch (InterruptedException cause) {
                            Thread.currentThread().interrupt();
                            throw new IOException(cause);
                        }
                        throw new IOException("body closed by deadline");
                    }

                    @Override
                    public void close() {
                        closeCount.incrementAndGet();
                        closed.countDown();
                    }
                };
        RedirectHttpClient client = new RedirectHttpClient(intermediate);
        var executor = Executors.newSingleThreadExecutor();
        var scheduler = Executors.newSingleThreadScheduledExecutor();
        try {
            var performer =
                    new RequestPerformer(
                            options(Duration.ofMillis(250)),
                            new CookieJar(),
                            client,
                            executor,
                            scheduler);
            var pending = performer.performAsync(request()).toCompletableFuture();
            assertTrue(reading.await(1, TimeUnit.SECONDS));
            CompletionException failure = assertThrows(CompletionException.class, pending::join);
            assertTrue(failure.getCause() instanceof HttpTimeoutException);
            assertEquals(1, closeCount.get());
            assertEquals(1, client.requests);
        } finally {
            executor.shutdownNow();
            scheduler.shutdownNow();
        }
    }

    @Test
    void executorRejectionClosesIntermediateBodyAndCancelsDeadline() {
        AtomicInteger closeCount = new AtomicInteger();
        var intermediate =
                new ByteArrayInputStream("intermediate".getBytes()) {
                    @Override
                    public void close() throws IOException {
                        closeCount.incrementAndGet();
                        super.close();
                    }
                };
        AtomicReference<ScheduledFuture<?>> scheduled = new AtomicReference<>();
        var scheduler =
                new ScheduledThreadPoolExecutor(1) {
                    @Override
                    public ScheduledFuture<?> schedule(Runnable task, long delay, TimeUnit unit) {
                        ScheduledFuture<?> future = super.schedule(task, delay, unit);
                        scheduled.set(future);
                        return future;
                    }
                };
        try {
            var performer =
                    new RequestPerformer(
                            options(Duration.ofSeconds(5)),
                            new CookieJar(),
                            new RedirectHttpClient(intermediate),
                            task -> {
                                throw new RejectedExecutionException("executor rejected body read");
                            },
                            scheduler);
            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> performer.performAsync(request()).toCompletableFuture().join());
            assertTrue(failure.getCause() instanceof RejectedExecutionException);
            assertEquals(1, closeCount.get());
            assertTrue(scheduled.get().isCancelled());
        } finally {
            scheduler.shutdownNow();
        }
    }

    private static HttpClientOptions options(Duration timeout) {
        return new HttpClientOptions(
                "http://example.test",
                timeout,
                1024,
                Map.of(),
                null,
                null,
                2,
                0,
                false,
                null,
                null,
                false);
    }

    private static HttpRequestSpec request() {
        return new HttpRequestSpec(ZLinkHttpMethod.GET, "/start", null, null, Map.of(), null, null);
    }

    private static final class RedirectHttpClient extends HttpClient {
        private final java.io.InputStream intermediate;
        private int requests;

        RedirectHttpClient(java.io.InputStream intermediate) {
            this.intermediate = intermediate;
        }

        @Override
        public Optional<CookieHandler> cookieHandler() {
            return Optional.empty();
        }

        @Override
        public Optional<Duration> connectTimeout() {
            return Optional.empty();
        }

        @Override
        public Redirect followRedirects() {
            return Redirect.NEVER;
        }

        @Override
        public Optional<ProxySelector> proxy() {
            return Optional.empty();
        }

        @Override
        public SSLContext sslContext() {
            return null;
        }

        @Override
        public SSLParameters sslParameters() {
            return new SSLParameters();
        }

        @Override
        public Optional<Authenticator> authenticator() {
            return Optional.empty();
        }

        @Override
        public Version version() {
            return Version.HTTP_1_1;
        }

        @Override
        public Optional<java.util.concurrent.Executor> executor() {
            return Optional.empty();
        }

        @Override
        public <T> HttpResponse<T> send(HttpRequest request, HttpResponse.BodyHandler<T> handler) {
            throw new UnsupportedOperationException();
        }

        @Override
        public <T> CompletableFuture<HttpResponse<T>> sendAsync(
                HttpRequest request, HttpResponse.BodyHandler<T> handler) {
            return CompletableFuture.completedFuture(response(request, requests++ == 0));
        }

        @Override
        public <T> CompletableFuture<HttpResponse<T>> sendAsync(
                HttpRequest request,
                HttpResponse.BodyHandler<T> handler,
                HttpResponse.PushPromiseHandler<T> pushPromiseHandler) {
            return sendAsync(request, handler);
        }

        @SuppressWarnings("unchecked")
        private <T> HttpResponse<T> response(HttpRequest request, boolean redirect) {
            return new HttpResponse<>() {
                @Override
                public int statusCode() {
                    return redirect ? 302 : 200;
                }

                @Override
                public HttpRequest request() {
                    return request;
                }

                @Override
                public Optional<HttpResponse<T>> previousResponse() {
                    return Optional.empty();
                }

                @Override
                public HttpHeaders headers() {
                    return HttpHeaders.of(
                            redirect ? Map.of("location", List.of("/result")) : Map.of(),
                            (name, value) -> true);
                }

                @Override
                public T body() {
                    return (T)
                            (redirect ? intermediate : new ByteArrayInputStream("ok".getBytes()));
                }

                @Override
                public Optional<SSLSession> sslSession() {
                    return Optional.empty();
                }

                @Override
                public URI uri() {
                    return request.uri();
                }

                @Override
                public Version version() {
                    return Version.HTTP_1_1;
                }
            };
        }
    }
}
