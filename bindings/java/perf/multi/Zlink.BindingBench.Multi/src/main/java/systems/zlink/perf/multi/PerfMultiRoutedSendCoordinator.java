/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.locks.LockSupport;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfSocketPollSet;

/** Coordinates async send admission on one application thread. */
final class PerfMultiTargetCoordinator {
    private PerfMultiTargetCoordinator() {
    }

    /**
     * Bounded post-deadline send-admission drain.
     *
     * <p>PERF_MULTI_TEST_POLICY.md &sect; 12.3 fixes the knob name
     * {@code PERF_MULTI_SEND_DRAIN_TIMEOUT_MS} and its 5000 ms default. The
     * drain starts no new submission and adds nothing to the RESULT
     * aggregate.</p>
     */
    static Duration sendDrainTimeout() {
        String raw = System.getenv("PERF_MULTI_SEND_DRAIN_TIMEOUT_MS");
        if (raw != null && !raw.isBlank()) {
            try {
                long parsed = Long.parseLong(raw.trim());
                if (parsed > 0) {
                    return Duration.ofMillis(parsed);
                }
            } catch (NumberFormatException ignored) {
                // Fall through to the policy default.
            }
        }
        return Duration.ofMillis(5000L);
    }

    static void run(int socketCount,
                    long activeEnd,
                    PerfSocketPollSet pollSet,
                    Submitter submitter,
                    ReplyDrainer replyDrainer,
                    ReplyDrainer teardownDrainer,
                    Duration terminalTimeout,
                    String label) {
        Objects.requireNonNull(pollSet, "pollSet");
        Objects.requireNonNull(replyDrainer, "replyDrainer");
        Objects.requireNonNull(teardownDrainer, "teardownDrainer");
        Objects.requireNonNull(terminalTimeout, "terminalTimeout");
        AdmissionRoundRobin admissions = new AdmissionRoundRobin(socketCount,
            activeEnd, submitter, true, false);

        long received = 0L;
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
                received += replyDrainer.drain(pollSet.readyIndexAt(readyOffset));
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

        System.err.println("CLIENT_DRAIN_DETAIL,enter,pending="
            + admissions.pendingCount() + ",submitted="
            + admissions.submittedCount() + ",admitted="
            + admissions.admittedCount() + ",received=" + received
            + ",outstanding=" + (admissions.admittedCount() - received)
            + ",label=" + label);
        long drainStart = System.nanoTime();
        long activeReceived = received;
        long drained = 0L;
        try {
            drained = admissions.awaitLatestWhileReceiving(terminalTimeout,
                label, activeReceived,
                waitMillis -> pollAndDrain(pollSet, teardownDrainer,
                    waitMillis));
        } finally {
            System.err.println("CLIENT_DRAIN_DETAIL,exit,pending="
                + admissions.pendingCount() + ",drained=" + drained
                + ",outstanding="
                + (admissions.admittedCount() - activeReceived - drained)
                + ",elapsed_ms="
                + ((System.nanoTime() - drainStart) / 1_000_000L)
                + ",label=" + label);
        }
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

    /** One bounded receive turn: polls once and consumes what is ready. */
    @FunctionalInterface
    interface TeardownReceiver {
        int receive(int timeoutMillis);
    }

    private static int pollAndDrain(PerfSocketPollSet pollSet,
                                    ReplyDrainer drainer,
                                    int timeoutMillis) {
        int readyCount = pollSet.poll(timeoutMillis);
        int drained = 0;
        for (int readyOffset = 0; readyOffset < readyCount; readyOffset++) {
            if (!pollSet.readyHasEventAt(readyOffset, PollEventFlags.POLLIN)) {
                continue;
            }
            drained += drainer.drain(pollSet.readyIndexAt(readyOffset));
        }
        return drained;
    }

    @FunctionalInterface
    interface ReplyDrainer {
        /** @return the number of replies consumed from this socket. */
        int drain(int socketIndex);
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
        private long submittedCount;
        // Every admitted request owes exactly one echo. Counted on the
        // terminal, so a rejected admission never owes one (C counts the
        // same way: `slot->replies` rises only on a successful send).
        private final java.util.concurrent.atomic.AtomicLong admittedCount =
            new java.util.concurrent.atomic.AtomicLong();

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
            submittedCount++;
            latestStages[index] = stage.handle((ignored, error) -> {
                if (error != null) {
                    recordFailure(error);
                } else {
                    admittedCount.incrementAndGet();
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
                submittedCount++;
                latestStages[index] = stage.handle((ignored, error) -> {
                    if (error != null) {
                        recordFailure(error);
                    } else {
                        admittedCount.incrementAndGet();
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

        /** Sockets whose latest admission has not reached its terminal. */
        int pendingCount() {
            int pending = 0;
            for (int index = 0; index < socketCount; index++) {
                if (states.get(index) == PENDING) {
                    pending++;
                }
            }
            return pending;
        }

        long submittedCount() {
            return submittedCount;
        }

        long admittedCount() {
            return admittedCount.get();
        }

        void awaitLatest(Duration timeout, String label) {
            List<CompletionStage<Void>> stages = latestStages();
            if (!stages.isEmpty()) {
                PerfMultiAsyncSendLoop.awaitAll(stages, timeout, label);
            }
        }

        /**
         * Awaits the outstanding send terminals while still receiving echoes.
         *
         * <p>A blocking wait here deadlocks the echo topology: the relay holds
         * one reply under Core admission and refuses to pull the next request
         * until it is admitted ({@code PerfMultiRoutedRelay.drainRequests}), so
         * a client receive queue left at its HWM stops that reply, and the
         * stopped relay in turn stops this client's own send admission. The C
         * echo client never stops receiving in its post-deadline drain
         * ({@code bindings/c/perf/multi/common/perf_multi_client_helpers.hpp},
         * drain loop after the active deadline), and .NET does the same in
         * {@code PerfMultiEchoReplyDrain.WaitAsync}.</p>
         *
         * <p>The window ends only when both halves are settled, exactly like
         * the C loop's {@code tracker_has_retained_sends ||
         * tracker_has_pending_replies}: the send terminals must be complete
         * <em>and</em> the echoes owed for the requests already admitted must
         * have been pulled. Leaving the owed echoes in the client's receive
         * queue is what strands the relay's last reply under admission, and
         * the relay then burns its own drain window after STOP. Replies
         * consumed here are past the active window, so they are discarded and
         * never reach the RESULT aggregate.</p>
         *
         * @param activeReceived echoes already received in the active window
         * @return the number of post-deadline replies consumed
         */
        long awaitLatestWhileReceiving(Duration timeout, String label,
                                       long activeReceived,
                                       TeardownReceiver receiver) {
            List<CompletionStage<Void>> stages = latestStages();
            CompletableFuture<Void> all = stages.isEmpty()
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.allOf(stages.stream()
                    .map(CompletionStage::toCompletableFuture)
                    .toArray(CompletableFuture[]::new));
            long deadline = System.nanoTime() + timeout.toNanos();
            long drained = 0L;
            while (!all.isDone()
                || admittedCount() - activeReceived - drained > 0L) {
                if (hasFailure()) {
                    // A failing send terminal ends the run; do not spend the
                    // window waiting for echoes that will never be owed.
                    break;
                }
                long remainingNanos = deadline - System.nanoTime();
                if (remainingNanos <= 0L) {
                    break;
                }
                int waitMillis = (int) Math.min(50L,
                    Math.max(1L, remainingNanos / 1_000_000L));
                drained += receiver.receive(waitMillis);
            }
            if (stages.isEmpty()) {
                return drained;
            }
            // The bounded window is unchanged; awaitAll only republishes the
            // terminal state, including the policy timeout message.
            long remainingNanos = deadline - System.nanoTime();
            PerfMultiAsyncSendLoop.awaitAll(stages,
                Duration.ofMillis(Math.max(1L, remainingNanos / 1_000_000L)),
                label);
            return drained;
        }

        private List<CompletionStage<Void>> latestStages() {
            List<CompletionStage<Void>> stages = new ArrayList<>(socketCount);
            for (CompletionStage<Void> stage : latestStages) {
                if (stage != null) {
                    stages.add(stage);
                }
            }
            return stages;
        }

        void throwIfFailed(String label) {
            Throwable cause = failure.get();
            if (cause != null) {
                // Zlink exceptions carry a null message, so the FAIL reason
                // would otherwise stop at this label. Name result and errno.
                throw new IllegalStateException(label + " failed:"
                    + PerfMultiRoutedRelay.describe(cause), cause);
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
