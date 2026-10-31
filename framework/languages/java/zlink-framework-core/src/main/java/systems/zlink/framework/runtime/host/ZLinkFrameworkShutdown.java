package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.execution.ZLinkTeardownExecutor;

import java.time.Instant;
import java.util.ArrayDeque;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

final class ZLinkFrameworkShutdown {
    private final Instant deadline;

    ZLinkFrameworkShutdown(Instant deadline) {
        this.deadline = deadline;
    }

    private final ArrayDeque<Supplier<CompletionStage<Void>>> actions = new ArrayDeque<>();

    /**
     * Registers a stage that closes a runtime resource. The next stage starts only after this one
     * returned, also after the host deadline expired: a later stage closes what an earlier one
     * still uses (the Core context under a mesh pump), and the expired deadline only stops stages
     * that wait for application progress.
     */
    void defer(String stage, Runnable action) {
        actions.push(() -> atStage(stage, () -> ZLinkTeardownExecutor.submit(action)));
    }

    void deferCloseStage(String stage, Supplier<CompletionStage<Void>> action) {
        actions.push(() -> atStage(stage, action));
    }

    void deferStage(String stage, Supplier<CompletionStage<Void>> action) {
        actions.push(() -> atStage(stage, () -> withinDeadline(action)));
    }

    private CompletionStage<Void> withinDeadline(Supplier<CompletionStage<Void>> action) {
        if (!Instant.now().isBefore(deadline)) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("host shutdown deadline expired"));
        }
        return action.get()
                .thenCompose(
                        ignored ->
                                Instant.now().isBefore(deadline)
                                        ? CompletableFuture.completedFuture(null)
                                        : CompletableFuture.failedFuture(
                                                new TimeoutException(
                                                        "host shutdown deadline expired")));
    }

    static CompletionStage<Void> atStage(String stage, Supplier<CompletionStage<Void>> action) {
        try {
            return action.get()
                    .exceptionallyCompose(
                            error ->
                                    CompletableFuture.failedFuture(
                                            new Failure(stage, unwrap(error))));
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(new Failure(stage, error));
        }
    }

    static final class Failure extends RuntimeException {
        private final String stage;

        Failure(String stage, Throwable cause) {
            super(cause.getMessage(), cause, true, false);
            this.stage = stage;
        }

        String stage() {
            return stage;
        }
    }

    CompletionStage<Void> closeAsync() {
        AtomicReference<Throwable> failure = new AtomicReference<>();
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        while (!actions.isEmpty()) {
            var action = actions.pop();
            chain = chain.thenCompose(ignored -> invoke(action, failure));
        }
        return chain.thenCompose(
                ignored ->
                        failure.get() == null
                                ? CompletableFuture.completedFuture(null)
                                : CompletableFuture.failedFuture(failure.get()));
    }

    private static CompletionStage<Void> invoke(
            Supplier<CompletionStage<Void>> action, AtomicReference<Throwable> failure) {
        try {
            return action.get()
                    .handle(
                            (ignored, error) -> {
                                if (error != null) {
                                    recordFailure(failure, unwrap(error));
                                }
                                return null;
                            });
        } catch (RuntimeException error) {
            recordFailure(failure, error);
            return CompletableFuture.completedFuture(null);
        }
    }

    private static Throwable unwrap(Throwable error) {
        Throwable value = error;
        while ((value instanceof CompletionException || value instanceof ExecutionException)
                && value.getCause() != null) {
            value = value.getCause();
        }
        return value;
    }

    private static void recordFailure(AtomicReference<Throwable> target, Throwable error) {
        Throwable first = target.get();
        if (first == null) {
            target.compareAndSet(null, error);
        } else {
            first.addSuppressed(error);
        }
    }
}
