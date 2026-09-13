/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicIntegerArray;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.perf.PerfSocketPollSet;

/** Coordinates public send results on one application submit thread. */
final class PerfMultiRoutedSendCoordinator {
    private PerfMultiRoutedSendCoordinator() {
    }

    /** Bounded post-deadline send-admission drain. */
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
        BackpressureCoordinator submissions = new BackpressureCoordinator(
            socketCount, activeEnd, submitter);

        long received = 0L;
        while (System.nanoTime() < activeEnd && !submissions.hasFailure()) {
            boolean submitted = submissions.submitRound();
            if (submissions.hasFailure()) {
                break;
            }

            // One public poller turn services both reply readiness and async
            // admission completions for every socket. Immediate OK never
            // blocks; only a turn with no eligible sender may wait briefly.
            int readyCount = pollSet.poll(submitted ? 0
                : pollWaitMillis(activeEnd));
            for (int readyOffset = 0; readyOffset < readyCount;
                 readyOffset++) {
                if (!pollSet.readyHasEventAt(readyOffset,
                        PollEventFlags.POLLIN)) {
                    continue;
                }
                received += replyDrainer.drain(pollSet.readyIndexAt(readyOffset));
            }
        }

        System.err.println("CLIENT_DRAIN_DETAIL,enter,pending="
            + submissions.pendingCount() + ",submitted="
            + submissions.submittedCount() + ",admitted="
            + submissions.admittedCount() + ",received=" + received
            + ",outstanding=" + (submissions.admittedCount() - received)
            + ",label=" + label);
        long drainStart = System.nanoTime();
        long activeReceived = received;
        long drained = 0L;
        try {
            drained = submissions.awaitPendingWhileReceiving(terminalTimeout,
                label, activeReceived,
                waitMillis -> pollAndDrain(pollSet, teardownDrainer,
                    waitMillis));
        } finally {
            System.err.println("CLIENT_DRAIN_DETAIL,exit,pending="
                + submissions.pendingCount() + ",drained=" + drained
                + ",outstanding="
                + (submissions.admittedCount() - activeReceived - drained)
                + ",elapsed_ms="
                + ((System.nanoTime() - drainStart) / 1_000_000L)
                + ",label=" + label);
        }
        submissions.throwIfFailed(label);
    }

    static void runAdmissions(int socketCount,
                              long activeEnd,
                              CompletionPoller completionPoller,
                              Submitter submitter,
                              Duration terminalTimeout,
                              String label) {
        Objects.requireNonNull(completionPoller, "completionPoller");
        Objects.requireNonNull(terminalTimeout, "terminalTimeout");
        BackpressureCoordinator submissions = new BackpressureCoordinator(
            socketCount, activeEnd, submitter);

        while (System.nanoTime() < activeEnd && !submissions.hasFailure()) {
            boolean submitted = submissions.submitRound();
            if (submissions.hasFailure()) {
                break;
            }
            completionPoller.poll(submitted ? 0 : pollWaitMillis(activeEnd));
        }

        submissions.awaitPending(terminalTimeout, label, completionPoller);
        submissions.throwIfFailed(label);
    }

    @FunctionalInterface
    interface CompletionPoller {
        void poll(int timeoutMillis);
    }

    @FunctionalInterface
    interface Submitter {
        SendSubmission submit(int socketIndex);
    }

    /** One bounded receive turn used only by post-deadline echo drain. */
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

    private static int pollWaitMillis(long deadline) {
        long remainingNanos = deadline - System.nanoTime();
        if (remainingNanos <= 0L) {
            return 0;
        }
        return (int) Math.min(50L,
            Math.max(1L, remainingNanos / 1_000_000L));
    }

    @FunctionalInterface
    interface ReplyDrainer {
        /** @return the number of replies consumed from this socket. */
        int drain(int socketIndex);
    }

    /**
     * Keeps state only for submissions that actually returned BACKPRESSURED.
     * The caller thread remains the sole submitter; completion callbacks only
     * make their socket available for a later poller turn.
     */
    static final class BackpressureCoordinator {
        private static final int AVAILABLE = 0;
        private static final int BACKPRESSURED = 1;

        private final int socketCount;
        private final long activeEnd;
        private final Submitter submitter;
        private final AtomicIntegerArray states;
        private final AtomicInteger pendingCount = new AtomicInteger();
        private final AtomicLong admittedCount = new AtomicLong();
        private final AtomicReference<Throwable> failure =
            new AtomicReference<>();
        private int roundRobinIndex;
        private long submittedCount;

        BackpressureCoordinator(int socketCount, long activeEnd,
                                Submitter submitter) {
            if (socketCount <= 0) {
                throw new IllegalArgumentException(
                    "socketCount must be greater than zero");
            }
            this.socketCount = socketCount;
            this.activeEnd = activeEnd;
            this.submitter = Objects.requireNonNull(submitter, "submitter");
            this.states = new AtomicIntegerArray(socketCount);
        }

        boolean submitRound() {
            if (hasFailure()) {
                return false;
            }
            boolean submitted = false;
            int start = roundRobinIndex;
            roundRobinIndex = (roundRobinIndex + 1) % socketCount;
            for (int attempt = 0; attempt < socketCount; attempt++) {
                if (System.nanoTime() >= activeEnd || hasFailure()) {
                    break;
                }
                int index = (start + attempt) % socketCount;
                if (states.get(index) != AVAILABLE) {
                    continue;
                }
                submitted |= submitOnce(index);
            }
            return submitted;
        }

        private boolean submitOnce(int index) {
            SendSubmission submission;
            try {
                submission = Objects.requireNonNull(submitter.submit(index),
                    "async send submission");
            } catch (Throwable error) {
                recordFailure(error);
                return false;
            }
            submittedCount++;
            if (!PerfMultiAsyncSendLoop.isBackpressured(submission)) {
                admittedCount.incrementAndGet();
                return true;
            }

            states.set(index, BACKPRESSURED);
            pendingCount.incrementAndGet();
            try {
                Objects.requireNonNull(submission.admitted(),
                    "backpressured admission stage")
                    .whenComplete((ignored, error) ->
                        completeAdmission(index, error));
            } catch (Throwable error) {
                completeAdmission(index, error);
            }
            return true;
        }

        private void completeAdmission(int index, Throwable error) {
            if (error != null) {
                recordFailure(error);
            } else {
                admittedCount.incrementAndGet();
            }
            states.set(index, AVAILABLE);
            pendingCount.decrementAndGet();
        }

        boolean hasFailure() {
            return failure.get() != null;
        }

        int pendingCount() {
            return pendingCount.get();
        }

        long submittedCount() {
            return submittedCount;
        }

        long admittedCount() {
            return admittedCount.get();
        }

        void awaitPending(Duration timeout, String label,
                          CompletionPoller completionPoller) {
            long deadline = System.nanoTime() + timeout.toNanos();
            while (pendingCount() > 0 && !hasFailure()) {
                int waitMillis = pollWaitMillis(deadline);
                if (waitMillis == 0) {
                    break;
                }
                completionPoller.poll(waitMillis);
            }
            if (pendingCount() > 0 && !hasFailure()) {
                throw new IllegalStateException(label + " timed out");
            }
        }

        /**
         * Awaits BACKPRESSURED admissions while continuing to consume echo
         * replies. Replies consumed here are outside the active window and do
         * not reach the RESULT aggregate.
         */
        long awaitPendingWhileReceiving(Duration timeout, String label,
                                         long activeReceived,
                                         TeardownReceiver receiver) {
            long deadline = System.nanoTime() + timeout.toNanos();
            long drained = 0L;
            while (pendingCount() > 0
                || admittedCount() - activeReceived - drained > 0L) {
                if (hasFailure()) {
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
            if (pendingCount() > 0 && !hasFailure()) {
                throw new IllegalStateException(label + " timed out");
            }
            return drained;
        }

        void throwIfFailed(String label) {
            Throwable cause = failure.get();
            if (cause != null) {
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
    }
}
