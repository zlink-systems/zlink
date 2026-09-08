/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Consumer;
import java.util.function.Predicate;

/**
 * Pending routed replies whose single sender awaits the preceding admission.
 *
 * <p>The C relay couples receive and send inside one turn: it keeps receiving
 * while a reply is backpressured, holds that reply as an immutable snapshot in
 * a FIFO, and retries it when the wait token reports WRITABLE
 * ({@code bindings/c/perf/multi/common/perf_multi_relay_server.hpp:331-431}).
 * This is the same shape on the public asynchronous submit, where the binding
 * owns the WRITABLE retry behind the returned stage.</p>
 *
 * <p>There is no application-side inflight window. The FIFO is unbounded and
 * Core admission alone paces it, so this adds no measurement cap (D-BP15).
 * One submit is outstanding at a time only because the next reply must not be
 * offered before the previous one has been admitted, exactly as in C.</p>
 */
final class PerfMultiRoutedReplyQueue<T> {
    private final Object lock = new Object();
    private final ArrayDeque<T> pending = new ArrayDeque<>();
    private final AtomicReference<Throwable> failure = new AtomicReference<>();
    private final Submitter<T> submitter;
    private final Consumer<T> disposer;
    private final Predicate<Throwable> ignorable;
    private boolean sending;
    private boolean pumping;

    PerfMultiRoutedReplyQueue(Submitter<T> submitter,
                              Consumer<T> disposer,
                              Predicate<Throwable> ignorable) {
        this.submitter = Objects.requireNonNull(submitter, "submitter");
        this.disposer = Objects.requireNonNull(disposer, "disposer");
        this.ignorable = Objects.requireNonNull(ignorable, "ignorable");
    }

    @FunctionalInterface
    interface Submitter<T> {
        /** Submits one reply; the submit consumes the reply it is given. */
        CompletionStage<Void> submit(T reply);
    }

    void enqueue(T reply) {
        Objects.requireNonNull(reply, "reply");
        synchronized (lock) {
            pending.addLast(reply);
            pump();
        }
    }

    boolean hasFailure() {
        return failure.get() != null;
    }

    Throwable failure() {
        return failure.get();
    }

    int pendingCount() {
        synchronized (lock) {
            return pending.size();
        }
    }

    boolean sending() {
        synchronized (lock) {
            return sending;
        }
    }

    /**
     * Waits, at most once and for at most {@code timeoutMillis}, until no
     * submit is outstanding.
     *
     * <p>The C relay refuses to offer a second reply while one wait token is
     * live ({@code perf_multi_relay_server.hpp:200-201}). Letting the caller
     * park on that condition keeps an un-forwarded reply backpressuring its
     * source through Core's receive queue instead of an application queue.</p>
     *
     * @return true when nothing is outstanding
     */
    boolean awaitIdle(long timeoutMillis) {
        synchronized (lock) {
            if (!sending) {
                return true;
            }
            try {
                lock.wait(timeoutMillis);
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                return false;
            }
            return !sending;
        }
    }

    /**
     * Waits for the outstanding admissions within a bounded window and then
     * releases whatever the window did not admit.
     *
     * <p>The C relay admits its retained reply after STOP before socket
     * teardown and keeps that window bounded
     * ({@code perf_multi_relay_server.hpp:527-600}).</p>
     *
     * @return true when the FIFO emptied before the deadline
     */
    boolean drain(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        long deadlineNanos = System.nanoTime() + timeout.toNanos();
        boolean drained;
        synchronized (lock) {
            pump();
            while (!hasFailure() && (sending || !pending.isEmpty())) {
                long remainingNanos = deadlineNanos - System.nanoTime();
                if (remainingNanos <= 0L) {
                    break;
                }
                try {
                    lock.wait(Math.max(1L, remainingNanos / 1_000_000L));
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    break;
                }
            }
            drained = !sending && pending.isEmpty();
            releasePendingLocked();
        }
        return drained;
    }

    /** Releases every reply the FIFO still owns. Caller holds {@code lock}. */
    private void releasePendingLocked() {
        for (T reply : pending) {
            disposer.accept(reply);
        }
        pending.clear();
    }

    // Caller holds `lock`. Submits one reply at a time; the completion of that
    // submit releases the next. Core settles an immediate admission inline on
    // this thread, so the re-entrancy guard keeps this loop the single driver
    // instead of recursing once per admitted reply.
    private void pump() {
        if (pumping) {
            return;
        }
        pumping = true;
        try {
            while (!sending && !hasFailure() && !pending.isEmpty()) {
                T reply = pending.pollFirst();
                sending = true;
                CompletionStage<Void> stage;
                try {
                    stage = Objects.requireNonNull(submitter.submit(reply),
                        "async submit stage");
                } catch (Throwable error) {
                    sending = false;
                    disposer.accept(reply);
                    // An ignorable failure (a route that went away) drops this
                    // reply and keeps the FIFO head moving, as the C relay does.
                    recordFailure(error);
                    continue;
                }
                stage.whenComplete((ignored, error) -> {
                    synchronized (lock) {
                        sending = false;
                        if (error != null) {
                            recordFailure(error);
                        }
                        lock.notifyAll();
                        pump();
                    }
                });
            }
        } finally {
            pumping = false;
            lock.notifyAll();
        }
    }

    private void recordFailure(Throwable error) {
        Throwable cause = PerfMultiAsyncSendLoop.completionCause(error);
        if (ignorable.test(cause)) {
            return;
        }
        failure.compareAndSet(null, cause);
    }
}
