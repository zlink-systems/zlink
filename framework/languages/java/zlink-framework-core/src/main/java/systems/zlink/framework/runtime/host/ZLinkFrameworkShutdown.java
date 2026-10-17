package systems.zlink.framework.runtime.host;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.execution.ZLinkTeardownDeadlineExceededException;
import systems.zlink.framework.runtime.internal.execution.ZLinkTeardownExecutor;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayDeque;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

final class ZLinkFrameworkShutdown {
    private final Instant deadline;

    ZLinkFrameworkShutdown(Instant deadline) {
        this.deadline = deadline;
    }

    private final ArrayDeque<Stage> actions = new ArrayDeque<>();

    private record Stage(
            String name,
            Supplier<CompletionStage<Void>> action,
            CompletableFuture<Void> completion) {
        Stage(String name, Supplier<CompletionStage<Void>> action) {
            this(name, action, new CompletableFuture<>());
        }
    }

    /**
     * Registers a stage that closes a runtime resource. The next stage starts only after this one
     * returned, also after the host deadline expired: a later stage closes what an earlier one
     * still uses (the Core context under a mesh pump), and the expired deadline only stops stages
     * that wait for application progress.
     */
    void defer(String stage, Runnable action) {
        deferStage(stage, () -> ZLinkTeardownExecutor.submit(action));
    }

    void deferStage(String stage, Supplier<CompletionStage<Void>> action) {
        actions.push(new Stage(stage, () -> withinDeadline(action.get())));
    }

    private CompletionStage<Void> withinDeadline(CompletionStage<Void> action) {
        long remaining = Duration.between(Instant.now(), deadline).toNanos();
        if (action.toCompletableFuture().isDone()) {
            return action.thenCompose(
                    ignored ->
                            remaining > 0L
                                    ? action
                                    : CompletableFuture.failedFuture(
                                            new ZLinkTeardownDeadlineExceededException()));
        }
        if (remaining <= 0L) {
            return CompletableFuture.failedFuture(new ZLinkTeardownDeadlineExceededException());
        }
        CompletableFuture<Void> completion = new CompletableFuture<>();
        action.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) completion.complete(null);
                    else completion.completeExceptionally(new CompletionException(failure));
                });
        return completion
                .orTimeout(
                        Math.max(0L, Duration.between(Instant.now(), deadline).toNanos()),
                        TimeUnit.NANOSECONDS)
                .exceptionallyCompose(
                        error ->
                                CompletableFuture.failedFuture(
                                        error instanceof TimeoutException
                                                ? new ZLinkTeardownDeadlineExceededException()
                                                : error));
    }

    static boolean hasTeardownFailure(Throwable error) {
        if (error == null) return false;
        boolean failed =
                !((error instanceof Failure
                                        || error instanceof CompletionException
                                        || error instanceof ExecutionException)
                                && error.getCause() != null)
                        && !(error instanceof ZLinkTeardownDeadlineExceededException)
                        && !(ZLinkFrameworkErrorOrigin.isFramework(error)
                                && error instanceof ZLinkFrameworkException failure
                                && (failure.kind() == ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED
                                        || failure.kind()
                                                == ZLinkFrameworkErrorKind.SHUTTING_DOWN));
        if (error.getCause() != null) {
            failed |= hasTeardownFailure(error.getCause());
        }
        for (Throwable suppressed : error.getSuppressed()) {
            failed |= hasTeardownFailure(suppressed);
        }
        return failed;
    }

    static CompletionStage<Void> atStage(String stage, Supplier<CompletionStage<Void>> action) {
        try {
            return action.get()
                    .exceptionallyCompose(
                            error ->
                                    CompletableFuture.failedFuture(
                                            new Failure(stage, unwrap(error))));
        } catch (RuntimeException | Error error) {
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

    /** Registers a drain step in execution order; the host owns its single deadline. */
    void appendStage(String stage, Supplier<CompletionStage<Void>> action) {
        actions.addLast(new Stage(stage, action));
    }

    Throwable deadlineFailure() {
        for (var stage : actions) {
            if (!stage.completion().isDone()) {
                return new Failure(stage.name(), new ZLinkTeardownDeadlineExceededException());
            }
            try {
                stage.completion().getNow(null);
            } catch (CompletionException failure) {
                return unwrap(failure);
            }
        }
        return null;
    }

    CompletionStage<Void> runAsync() {
        return execute(null);
    }

    CompletionStage<Void> closeAsync() {
        return execute(new AtomicReference<>());
    }

    private CompletionStage<Void> execute(AtomicReference<Throwable> failure) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        var stages = actions.iterator();
        while (stages.hasNext()) {
            var stage = stages.next();
            if (failure != null) stages.remove();
            chain = chain.thenCompose(ignored -> invoke(stage, failure));
        }
        return failure == null
                ? chain
                : chain.thenCompose(
                        ignored ->
                                failure.get() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : CompletableFuture.failedFuture(failure.get()));
    }

    private static CompletionStage<Void> invoke(Stage stage, AtomicReference<Throwable> failure) {
        atStage(stage.name(), stage.action())
                .whenComplete(
                        (ignored, error) -> {
                            if (error == null) stage.completion().complete(null);
                            else stage.completion().completeExceptionally(unwrap(error));
                        });
        return failure == null
                ? stage.completion()
                : stage.completion()
                        .handle(
                                (ignored, error) -> {
                                    if (error != null) recordFailure(failure, unwrap(error));
                                    return null;
                                });
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
