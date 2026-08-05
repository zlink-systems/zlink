/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import java.io.IOException;
import java.io.UncheckedIOException;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.httpclient.internal.RequestPerformer.RawResult;

/**
 * Applies the wrapper's retry policy as an async {@link CompletionStage} chain: only retriable
 * transport failures ({@link IOException}, including {@code HttpTimeoutException}) are retried at a
 * fixed delay, up to {@code maxRetries}. Separated from {@link HttpClientRuntime} so the retry
 * policy is independent of client construction.
 */
final class RetryPolicy {

    // Exponential backoff with full jitter: base 50ms, doubling per attempt, capped at 1s.
    // Fixed delays synchronize retries from many clients against an ailing server.
    private static long delayMillisFor(int attempt) {
        long ceiling = Math.min(1000L, 50L << Math.min(attempt, 5));
        return java.util.concurrent.ThreadLocalRandom.current().nextLong(ceiling + 1);
    }

    private final Executor executor;

    RetryPolicy(Executor executor) {
        this.executor = executor;
    }

    CompletionStage<RawResult> execute(int maxRetries, Supplier<CompletionStage<RawResult>> perform) {
        return attempt(0, maxRetries, perform);
    }

    private CompletionStage<RawResult> attempt(int attempt, int maxRetries, Supplier<CompletionStage<RawResult>> perform) {
        return perform.get().exceptionallyCompose(error -> {
            Throwable cause = unwrap(error);
            if (isRetriable(cause) && attempt < maxRetries) {
                Executor delayed = CompletableFuture.delayedExecutor(delayMillisFor(attempt), TimeUnit.MILLISECONDS, executor);
                return CompletableFuture.supplyAsync(() -> null, delayed)
                    .thenCompose(ignored -> attempt(attempt + 1, maxRetries, perform));
            }
            return CompletableFuture.failedFuture(
                cause instanceof ZLinkFrameworkException
                    ? cause
                    : new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        cause.getMessage(),
                        cause));
        });
    }

    private static boolean isRetriable(Throwable cause) {
        // Transport failures and timeouts (HttpTimeoutException extends IOException) are retriable,
        // including body-read failures surfaced as UncheckedIOException by ResponseBodyReader and
        // body-read timeouts surfaced as TimeoutException by orTimeout.
        return cause instanceof IOException
            || cause instanceof UncheckedIOException
            || cause instanceof TimeoutException;
    }

    private static Throwable unwrap(Throwable error) {
        Throwable cause = error;
        while ((cause instanceof CompletionException) && cause.getCause() != null) {
            cause = cause.getCause();
        }
        return cause;
    }
}
