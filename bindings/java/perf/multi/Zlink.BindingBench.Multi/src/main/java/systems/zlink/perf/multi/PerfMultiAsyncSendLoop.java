/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.sockets.SubmitResult;

/** Drives one public asynchronous send chain per multi-client socket. */
final class PerfMultiAsyncSendLoop {
    private PerfMultiAsyncSendLoop() {
    }

    static CompletionStage<Void> start(long activeEnd,
                                       CompletionStage<Void> startGate,
                                       Submitter submitter) {
        Loop loop = new Loop(activeEnd, startGate, submitter);
        loop.resume();
        return loop.completion;
    }

    static Duration remainingTimeout(long activeEnd) {
        long remainingNanos = activeEnd - System.nanoTime();
        long millis = Math.max(1L,
            (Math.max(1L, remainingNanos) + 999_999L) / 1_000_000L);
        return Duration.ofMillis(millis);
    }

    static void awaitAll(List<? extends CompletionStage<Void>> stages,
                         Duration timeout, String label) {
        List<CompletableFuture<Void>> futures = new ArrayList<>(stages.size());
        for (CompletionStage<Void> stage : stages) {
            futures.add(stage.toCompletableFuture());
        }
        try {
            CompletableFuture.allOf(futures.toArray(CompletableFuture[]::new))
                .get(timeout.toMillis(), TimeUnit.MILLISECONDS);
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException(label + " interrupted", error);
        } catch (TimeoutException error) {
            throw new IllegalStateException(label + " timed out", error);
        } catch (ExecutionException error) {
            throw rethrow(label, completionCause(error));
        }
    }

    @FunctionalInterface
    interface Submitter {
        CompletionStage<Void> submit();
    }

    private static final class Loop {
        private final long activeEnd;
        private final CompletionStage<Void> startGate;
        private final Submitter submitter;
        private final CompletableFuture<Void> completion =
            new CompletableFuture<>();

        private Loop(long activeEnd, CompletionStage<Void> startGate,
                     Submitter submitter) {
            this.activeEnd = activeEnd;
            this.startGate = Objects.requireNonNull(startGate, "startGate");
            this.submitter = Objects.requireNonNull(submitter, "submitter");
        }

        private void resume() {
            if (completion.isDone()) {
                return;
            }
            if (System.nanoTime() >= activeEnd) {
                completion.complete(null);
                return;
            }

            CompletionStage<Void> stage;
            try {
                stage = Objects.requireNonNull(submitter.submit(),
                    "async submit stage");
            } catch (Throwable error) {
                completeFailure(error);
                return;
            }

            // Always resume through the language async runtime, including
            // Core's immediate-admission case. An inline loop could otherwise
            // let the first socket consume the whole active window before the
            // remaining client tasks are started.
            stage.whenComplete((ignored, error) -> {
                if (error != null) {
                    completeFailure(completionCause(error));
                } else {
                    startGate.whenCompleteAsync((gateIgnored, gateError) -> {
                        if (gateError != null) {
                            completeFailure(completionCause(gateError));
                        } else {
                            resume();
                        }
                    });
                }
            });
        }

        private void completeFailure(Throwable error) {
            Throwable cause = completionCause(error);
            if (System.nanoTime() >= activeEnd
                && cause instanceof ZlinkSubmitException submit
                && (submit.getResult() == SubmitResult.NOT_ADMITTED
                    || submit.getResult() == SubmitResult.NOT_CONNECTED
                    || submit.getResult() == SubmitResult.TERMINATED)) {
                completion.complete(null);
                return;
            }
            completion.completeExceptionally(cause);
        }
    }

    private static IllegalStateException rethrow(String label,
                                                  Throwable cause) {
        return new IllegalStateException(label + " failed", cause);
    }

    static Throwable completionCause(Throwable error) {
        Throwable current = error;
        while ((current instanceof CompletionException
                || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }
}
