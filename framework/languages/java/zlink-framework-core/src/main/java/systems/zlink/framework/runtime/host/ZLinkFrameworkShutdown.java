package systems.zlink.framework.runtime.host;
import java.util.ArrayDeque;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

import systems.zlink.contracts.errors.ZlinkCloseException;

final class ZLinkFrameworkShutdown {
    private static final long ACTION_TIMEOUT_SECONDS = 2;
    private final ArrayDeque<
        Supplier<CompletionStage<Void>>> actions =
        new ArrayDeque<>();

    void defer(String stage, Runnable action) {
        deferStage(stage, () -> ZLinkTeardownExecutor.submit(action));
    }

    void deferStage(String stage, Supplier<CompletionStage<Void>> action) {
        actions.push(() -> atStage(stage, action));
    }

    static CompletionStage<Void> atStage(
        String stage, Supplier<CompletionStage<Void>> action) {
        try {
            return action.get().exceptionallyCompose(error ->
                CompletableFuture.failedFuture(new Failure(stage, unwrap(error))));
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
        AtomicReference<Throwable> failure =
            new AtomicReference<>();
        CompletionStage<Void> chain =
            CompletableFuture.completedFuture(null);
        while (!actions.isEmpty()) {
            var action = actions.pop();
            chain = chain.thenCompose(ignored -> invoke(action, failure));
        }
        return chain.thenCompose(ignored -> failure.get() == null
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(failure.get()));
    }

    private static CompletionStage<Void> invoke(
        Supplier<CompletionStage<Void>> action,
        AtomicReference<Throwable> failure) {
        try {
            return action.get()
                .toCompletableFuture()
                .completeOnTimeout(
                    null,
                    ACTION_TIMEOUT_SECONDS,
                    TimeUnit.SECONDS)
                .handle((ignored, error) -> {
                if (error != null && !(unwrap(error).getCause() instanceof ZlinkCloseException)) {
                    recordFailure(failure, unwrap(error));
                }
                return null;
                });
        } catch (ZlinkCloseException ignored) {
            return CompletableFuture.completedFuture(null);
        } catch (RuntimeException error) {
            recordFailure(failure, error);
            return CompletableFuture.completedFuture(null);
        }
    }

    private static Throwable unwrap(Throwable error) {
        Throwable value = error;
        while ((value instanceof CompletionException
            || value instanceof ExecutionException)
            && value.getCause() != null) {
            value = value.getCause();
        }
        return value;
    }

    private static void recordFailure(
        AtomicReference<Throwable> target,
        Throwable error) {
        Throwable first = target.get();
        if (first == null) {
            target.compareAndSet(null, error);
        } else {
            first.addSuppressed(error);
        }
    }
}
