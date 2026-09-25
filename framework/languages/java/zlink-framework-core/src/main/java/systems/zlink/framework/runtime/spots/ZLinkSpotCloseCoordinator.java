package systems.zlink.framework.runtime.spots;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Consumer;
import java.util.function.Supplier;

/** Runs one target owner's Close against one object generation. */
final class ZLinkSpotCloseCoordinator {
    static final class UncertainCommitFailure extends RuntimeException {
        UncertainCommitFailure(Throwable writeFailure, Throwable readFailure) {
            super("Spot Close authority commit could not be confirmed", writeFailure);
            addSuppressed(readFailure);
        }
    }

    static boolean isUncertainCommit(Throwable failure) {
        while (failure != null) {
            if (failure instanceof UncertainCommitFailure) {
                return true;
            }
            failure = failure.getCause();
        }
        return false;
    }

    record Step(Supplier<CompletionStage<Void>> operation, boolean onClosing) {
        Step {
            Objects.requireNonNull(operation, "operation");
        }

        static Step operation(Supplier<CompletionStage<Void>> operation) {
            return new Step(operation, false);
        }

        static Step onClosing(Supplier<CompletionStage<Void>> operation) {
            return new Step(operation, true);
        }
    }

    private final Supplier<CompletionStage<Boolean>> commit;
    private final List<Step> steps;
    private final Executor ownerExecutor;
    private final Consumer<Throwable> closingFailure;
    private int next = -1;
    private boolean running;
    private CompletableFuture<Boolean> attempt;
    private Throwable callbackFailure;

    ZLinkSpotCloseCoordinator(
            Supplier<CompletionStage<Boolean>> commit,
            List<Step> steps,
            Executor ownerExecutor,
            Consumer<Throwable> closingFailure) {
        this.commit = Objects.requireNonNull(commit, "commit");
        this.steps = List.copyOf(steps);
        this.ownerExecutor = Objects.requireNonNull(ownerExecutor, "ownerExecutor");
        this.closingFailure = Objects.requireNonNull(closingFailure, "closingFailure");
    }

    synchronized boolean committed() {
        return next >= 0;
    }

    synchronized void markCommitted() {
        if (next < 0) {
            next = 0;
        }
    }

    synchronized boolean finished() {
        return next == steps.size();
    }

    synchronized CompletionStage<Boolean> close() {
        if (running) {
            return attempt;
        }
        if (finished()) {
            return callbackFailure == null
                    ? CompletableFuture.completedFuture(true)
                    : CompletableFuture.failedFuture(callbackFailure);
        }
        running = true;
        attempt = new CompletableFuture<>();
        CompletableFuture<Boolean> result = attempt;
        advance(result, false);
        return result;
    }

    private void advance(CompletableFuture<Boolean> result, boolean background) {
        int position;
        synchronized (this) {
            position = next;
        }
        if (position < 0) {
            CompletionStage<Boolean> committed;
            try {
                committed = commit.get();
            } catch (Throwable failure) {
                fail(result, failure, background);
                return;
            }
            committed.whenComplete(
                    (value, failure) -> {
                        if (failure != null) {
                            fail(result, failure, background);
                        } else if (!Boolean.TRUE.equals(value)) {
                            finish(result, false);
                        } else {
                            markCommitted();
                            advance(result, background);
                        }
                    });
            return;
        }
        if (position == steps.size()) {
            if (callbackFailure == null) {
                finish(result, true);
            } else {
                fail(result, callbackFailure, true);
            }
            return;
        }
        Step step = steps.get(position);
        CompletionStage<Void> operation;
        try {
            operation = step.operation().get();
        } catch (Throwable failure) {
            operation = CompletableFuture.failedFuture(failure);
        }
        operation.whenComplete(
                (ignored, failure) -> {
                    if (failure != null && !step.onClosing()) {
                        fail(result, failure, background);
                        return;
                    }
                    synchronized (this) {
                        if (failure != null) {
                            callbackFailure = failure;
                        }
                        next = position + 1;
                    }
                    if (failure != null) {
                        try {
                            closingFailure.accept(failure);
                        } catch (Throwable diagnosticsFailure) {
                            failure.addSuppressed(diagnosticsFailure);
                        }
                    }
                    advance(result, background);
                });
    }

    private synchronized void finish(CompletableFuture<Boolean> result, boolean value) {
        running = false;
        attempt = null;
        result.complete(value);
    }

    private void fail(CompletableFuture<Boolean> result, Throwable failure, boolean background) {
        boolean resume;
        synchronized (this) {
            running = false;
            attempt = null;
            resume = next >= 0 && next < steps.size() && !background;
        }
        result.completeExceptionally(failure);
        if (resume) {
            ownerExecutor.execute(
                    () -> {
                        CompletableFuture<Boolean> continuation;
                        synchronized (this) {
                            if (running || finished()) {
                                return;
                            }
                            running = true;
                            continuation = new CompletableFuture<>();
                            attempt = continuation;
                        }
                        advance(continuation, true);
                    });
        }
    }
}
