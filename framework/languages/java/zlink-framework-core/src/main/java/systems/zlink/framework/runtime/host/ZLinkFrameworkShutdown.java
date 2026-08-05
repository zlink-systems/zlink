package systems.zlink.framework.runtime.host;

import systems.zlink.contracts.errors.ZlinkCloseException;

final class ZLinkFrameworkShutdown {
    private static final long ACTION_TIMEOUT_SECONDS = 2;
    private final java.util.ArrayDeque<
        java.util.function.Supplier<java.util.concurrent.CompletionStage<Void>>> actions =
        new java.util.ArrayDeque<>();

    void defer(Runnable action) {
        actions.push(() -> ZLinkTeardownExecutor.submit(action));
    }

    void deferStage(java.util.function.Supplier<java.util.concurrent.CompletionStage<Void>> action) {
        actions.push(action);
    }

    java.util.concurrent.CompletionStage<Void> closeAsync() {
        java.util.concurrent.atomic.AtomicReference<RuntimeException> failure =
            new java.util.concurrent.atomic.AtomicReference<>();
        java.util.concurrent.CompletionStage<Void> chain =
            java.util.concurrent.CompletableFuture.completedFuture(null);
        while (!actions.isEmpty()) {
            var action = actions.pop();
            chain = chain.thenCompose(ignored -> invoke(action, failure));
        }
        return chain.thenCompose(ignored -> failure.get() == null
                ? java.util.concurrent.CompletableFuture.completedFuture(null)
                : java.util.concurrent.CompletableFuture.failedFuture(failure.get()));
    }

    private static java.util.concurrent.CompletionStage<Void> invoke(
        java.util.function.Supplier<java.util.concurrent.CompletionStage<Void>> action,
        java.util.concurrent.atomic.AtomicReference<RuntimeException> failure) {
        try {
            return action.get()
                .toCompletableFuture()
                .completeOnTimeout(
                    null,
                    ACTION_TIMEOUT_SECONDS,
                    java.util.concurrent.TimeUnit.SECONDS)
                .handle((ignored, error) -> {
                if (error != null && !(unwrap(error) instanceof ZlinkCloseException)) {
                    recordFailure(failure, unwrap(error));
                }
                return null;
                });
        } catch (ZlinkCloseException ignored) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        } catch (RuntimeException error) {
            recordFailure(failure, error);
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
    }

    private static RuntimeException unwrap(Throwable error) {
        Throwable value = error;
        while ((value instanceof java.util.concurrent.CompletionException
            || value instanceof java.util.concurrent.ExecutionException)
            && value.getCause() != null) {
            value = value.getCause();
        }
        return value instanceof RuntimeException runtime
            ? runtime
            : new RuntimeException(value);
    }

    private static void recordFailure(
        java.util.concurrent.atomic.AtomicReference<RuntimeException> target,
        RuntimeException error) {
        RuntimeException first = target.get();
        if (first == null) {
            target.compareAndSet(null, error);
        } else {
            first.addSuppressed(error);
        }
    }
}
