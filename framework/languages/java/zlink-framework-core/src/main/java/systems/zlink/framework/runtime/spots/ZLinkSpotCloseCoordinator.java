package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
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
    private final Consumer<Throwable> closingFailure;
    // -1 until the Closing transition is committed; then the index of the next step to run.
    private int next = -1;
    private CompletableFuture<Boolean> attempt;

    ZLinkSpotCloseCoordinator(
            Supplier<CompletionStage<Boolean>> commit,
            List<Step> steps,
            Consumer<Throwable> closingFailure) {
        this.commit = Objects.requireNonNull(commit, "commit");
        this.steps = List.copyOf(steps);
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

    /** Starts this Close and shares its final result with callers of the same generation. */
    CompletionStage<Boolean> close() {
        CompletableFuture<Boolean> started;
        synchronized (this) {
            if (attempt != null) {
                return attempt;
            }
            attempt = started = new CompletableFuture<>();
        }
        advance(started);
        return started;
    }

    private void advance(CompletableFuture<Boolean> result) {
        int position;
        synchronized (this) {
            position = next;
        }
        if (position < 0) {
            CompletionStage<Boolean> committed;
            try {
                committed = commit.get();
            } catch (Throwable failure) {
                end(result, null, failure);
                return;
            }
            ZLinkSerialExecutionQueue.yieldCurrent(committed)
                    .whenComplete(
                            (value, failure) -> {
                                if (failure != null) {
                                    end(result, null, failure);
                                } else if (!Boolean.TRUE.equals(value)) {
                                    end(result, false, null);
                                } else {
                                    markCommitted();
                                    advance(result);
                                }
                            });
            return;
        }
        if (position == steps.size()) {
            end(result, true, null);
            return;
        }
        Step step = steps.get(position);
        CompletionStage<Void> operation;
        try {
            operation = step.operation().get();
        } catch (Throwable failure) {
            operation = CompletableFuture.failedFuture(failure);
        }
        ZLinkSerialExecutionQueue.yieldCurrent(operation)
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure != null && !step.onClosing()) {
                                end(result, null, failure);
                                return;
                            }
                            if (failure != null) {
                                // OnClosing failure is diagnostics only; cleanup continues.
                                try {
                                    closingFailure.accept(failure);
                                } catch (Throwable diagnosticsFailure) {
                                    failure.addSuppressed(diagnosticsFailure);
                                }
                            }
                            synchronized (this) {
                                next = position + 1;
                            }
                            advance(result);
                        });
    }

    private void end(CompletableFuture<Boolean> result, Boolean value, Throwable failure) {
        if (failure != null) {
            synchronized (this) {
                if (attempt == result) {
                    attempt = null;
                }
            }
        }
        if (failure == null) {
            result.complete(value);
        } else {
            result.completeExceptionally(failure);
        }
    }
}
