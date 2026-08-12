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

    void defer(Runnable action) {
        actions.push(() -> ZLinkTeardownExecutor.submit(action));
    }

    void deferStage(Supplier<CompletionStage<Void>> action) {
        actions.push(action);
    }

    CompletionStage<Void> closeAsync() {
        AtomicReference<RuntimeException> failure =
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
        AtomicReference<RuntimeException> failure) {
        try {
            return action.get()
                .toCompletableFuture()
                .completeOnTimeout(
                    null,
                    ACTION_TIMEOUT_SECONDS,
                    TimeUnit.SECONDS)
                .handle((ignored, error) -> {
                if (error != null && !(unwrap(error) instanceof ZlinkCloseException)) {
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

    private static RuntimeException unwrap(Throwable error) {
        Throwable value = error;
        while ((value instanceof CompletionException
            || value instanceof ExecutionException)
            && value.getCause() != null) {
            value = value.getCause();
        }
        return value instanceof RuntimeException runtime
            ? runtime
            : new RuntimeException(value);
    }

    private static void recordFailure(
        AtomicReference<RuntimeException> target,
        RuntimeException error) {
        RuntimeException first = target.get();
        if (first == null) {
            target.compareAndSet(null, error);
        } else {
            first.addSuppressed(error);
        }
    }
}
