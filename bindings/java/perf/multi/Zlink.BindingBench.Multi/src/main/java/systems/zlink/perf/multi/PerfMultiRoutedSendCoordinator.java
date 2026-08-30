/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfSocketPollSet;

/** Coordinates async send admission on one application thread. */
final class PerfMultiRoutedSendCoordinator {
    private PerfMultiRoutedSendCoordinator() {
    }

    static void run(int socketCount,
                    long activeEnd,
                    PerfSocketPollSet pollSet,
                    Submitter submitter,
                    ReplyDrainer replyDrainer,
                    Duration terminalTimeout,
                    String label) {
        Objects.requireNonNull(pollSet, "pollSet");
        Objects.requireNonNull(replyDrainer, "replyDrainer");
        Objects.requireNonNull(terminalTimeout, "terminalTimeout");
        AdmissionRoundRobin admissions = new AdmissionRoundRobin(socketCount,
            activeEnd, submitter, true, false);

        while (System.nanoTime() < activeEnd && !admissions.hasFailure()) {
            boolean submitted = admissions.submitRound();
            if (admissions.hasFailure()) {
                break;
            }

            // Reply readiness must never gate the next send admission. Poll
            // replies without blocking, then park only on the completion
            // signal for the same socket's previous admission.
            int readyCount = pollSet.poll(0);
            boolean drainedReply = false;
            for (int readyOffset = 0; readyOffset < readyCount;
                 readyOffset++) {
                if (!pollSet.readyHasEventAt(readyOffset,
                        PollEventFlags.POLLIN)) {
                    continue;
                }
                replyDrainer.drain(pollSet.readyIndexAt(readyOffset));
                drainedReply = true;
            }
            if (!submitted && !drainedReply
                && !admissions.awaitAvailability(activeEnd)) {
                if (Thread.currentThread().isInterrupted()) {
                    throw new IllegalStateException(label + " interrupted");
                }
                break;
            }
        }

        admissions.awaitLatest(terminalTimeout, label);
        admissions.throwIfFailed(label);
    }

    static void runAdmissions(int socketCount,
                              long activeEnd,
                              Submitter submitter,
                              Duration terminalTimeout,
                              String label) {
        Objects.requireNonNull(terminalTimeout, "terminalTimeout");
        AdmissionRoundRobin admissions = new AdmissionRoundRobin(socketCount,
            activeEnd, submitter, true, true);

        while (System.nanoTime() < activeEnd && !admissions.hasFailure()) {
            if (admissions.submitRound()) {
                continue;
            }
            if (!admissions.awaitAvailability(activeEnd)) {
                if (Thread.currentThread().isInterrupted()) {
                    throw new IllegalStateException(label + " interrupted");
                }
                break;
            }
        }

        admissions.awaitLatest(terminalTimeout, label);
        admissions.throwIfFailed(label);
    }

    @FunctionalInterface
    interface Submitter {
        CompletionStage<Void> submit(int socketIndex);
    }

    @FunctionalInterface
    interface ReplyDrainer {
        void drain(int socketIndex);
    }

    /**
     * Owns only admission state. Completion threads publish terminal state;
     * the caller of {@link #submitRound()} remains the sole submitter.
     */
    static final class AdmissionRoundRobin {
        private static final int AVAILABLE = 0;
        private static final int PENDING = 1;

        private final int socketCount;
        private final long activeEnd;
        private final Submitter submitter;
        private final AtomicIntegerArray states;
        private final CompletionStage<Void>[] latestStages;
        private final Thread coordinatorThread;
        private final boolean signalCompletions;
        private final boolean burstInlineTerminals;
        private final AtomicReference<Throwable> failure =
            new AtomicReference<>();
        private int roundRobinIndex;

        AdmissionRoundRobin(int socketCount, long activeEnd,
                            Submitter submitter) {
            this(socketCount, activeEnd, submitter, false, false);
        }

        @SuppressWarnings("unchecked")
        AdmissionRoundRobin(int socketCount, long activeEnd,
                            Submitter submitter,
                            boolean signalCompletions,
                            boolean burstInlineTerminals) {
            if (socketCount <= 0) {
                throw new IllegalArgumentException(
                    "socketCount must be greater than zero");
            }
            this.socketCount = socketCount;
            this.activeEnd = activeEnd;
            this.submitter = Objects.requireNonNull(submitter, "submitter");
            this.states = new AtomicIntegerArray(socketCount);
            this.latestStages = (CompletionStage<Void>[])
                new CompletionStage<?>[socketCount];
            this.signalCompletions = signalCompletions;
            this.burstInlineTerminals = burstInlineTerminals;
            this.coordinatorThread = signalCompletions
                ? Thread.currentThread() : null;
        }

        boolean submitRound() {
            if (hasFailure()) {
                return false;
            }
            boolean submitted = false;
            int start = roundRobinIndex;
            roundRobinIndex = (roundRobinIndex + 1) % socketCount;
            for (int attempt = 0; attempt < socketCount; attempt++) {
                if (hasFailure()) {
                    break;
                }
                int index = (start + attempt) % socketCount;
                submitted |= burstInlineTerminals
                    ? submitUntilPending(index)
                    : submitOnce(index);
            }
            return submitted;
        }

        /**
         * Submits at most once per socket in this round. An inline terminal
         * makes the socket eligible again in the next round; it does not
         * create an inflight-one window. This mirrors the C runner's fair
         * one-submit-per-writable-socket round before its next drain pass.
         */
        private boolean submitOnce(int index) {
            if (System.nanoTime() >= activeEnd || hasFailure()
                || !states.compareAndSet(index, AVAILABLE, PENDING)) {
                return false;
            }

            CompletionStage<Void> stage;
            try {
                stage = Objects.requireNonNull(submitter.submit(index),
                    "async submit stage");
            } catch (Throwable error) {
                recordFailure(error);
                states.set(index, AVAILABLE);
                return false;
            }
            latestStages[index] = stage.handle((ignored, error) -> {
                if (error != null) {
                    recordFailure(error);
                }
                states.set(index, AVAILABLE);
                if (signalCompletions) {
                    LockSupport.unpark(coordinatorThread);
                }
                return null;
            });
            return true;
        }

        private boolean submitUntilPending(int index) {
            boolean submitted = false;
            while (System.nanoTime() < activeEnd && !hasFailure()) {
                if (!states.compareAndSet(index, AVAILABLE, PENDING)) {
                    return submitted;
                }
                CompletionStage<Void> stage;
                try {
                    stage = Objects.requireNonNull(submitter.submit(index),
                        "async submit stage");
                } catch (Throwable error) {
                    recordFailure(error);
                    states.set(index, AVAILABLE);
                    return submitted;
                }
                latestStages[index] = stage.handle((ignored, error) -> {
                    if (error != null) {
                        recordFailure(error);
                    }
                    states.set(index, AVAILABLE);
                    if (signalCompletions) {
                        LockSupport.unpark(coordinatorThread);
                    }
                    return null;
                });
                submitted = true;
                if (states.get(index) == PENDING) {
                    return true;
                }
            }
            return submitted;
        }

        boolean awaitAvailability(long deadline) {
            if (!signalCompletions) {
                throw new IllegalStateException(
                    "completion signaling is not enabled");
            }
            while (!hasFailure() && !hasAvailable()) {
                long remainingNanos = deadline - System.nanoTime();
                if (remainingNanos <= 0L) {
                    return false;
                }
                LockSupport.parkNanos(this, remainingNanos);
                if (Thread.currentThread().isInterrupted()) {
                    return false;
                }
            }
            return true;
        }

        boolean hasFailure() {
            return failure.get() != null;
        }

        void awaitLatest(Duration timeout, String label) {
            List<CompletionStage<Void>> stages = new ArrayList<>(socketCount);
            for (CompletionStage<Void> stage : latestStages) {
                if (stage != null) {
                    stages.add(stage);
                }
            }
            if (!stages.isEmpty()) {
                PerfMultiAsyncSendLoop.awaitAll(stages, timeout, label);
            }
        }

        void throwIfFailed(String label) {
            Throwable cause = failure.get();
            if (cause != null) {
                throw new IllegalStateException(label + " failed", cause);
            }
        }

        private void recordFailure(Throwable error) {
            Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
            if (System.nanoTime() >= activeEnd
                && cause instanceof ZlinkSubmitException submit
                && (submit.getResult() == SubmitResult.NOT_ADMITTED
                    || submit.getResult() == SubmitResult.NOT_CONNECTED
                    || submit.getResult() == SubmitResult.TERMINATED)) {
                return;
            }
            failure.compareAndSet(null, cause);
        }

        private boolean hasAvailable() {
            for (int index = 0; index < socketCount; index++) {
                if (states.get(index) == AVAILABLE) {
                    return true;
                }
            }
            return false;
        }
    }
}
