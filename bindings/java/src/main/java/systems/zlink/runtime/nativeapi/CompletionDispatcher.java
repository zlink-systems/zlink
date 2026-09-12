/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.nativeapi;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.Objects;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;

/**
 * Context-scoped handoff from foreign Core callbacks to Java continuations.
 *
 * <p>A bounded shared worker pool executes lightweight socket-local serial
 * lanes. Each lane submits only one task at a time and resubmits its successor
 * after completion, preserving per-socket FIFO order without static-stripe
 * head-of-line blocking or one platform thread per socket. Context close marks
 * the dispatcher closed after Core quiescence; worker shutdown is deferred
 * until every already-active lane has drained.
 */
public final class CompletionDispatcher implements AutoCloseable {
    private static final int MAX_CONTEXT_WORKERS = 16;
    private final Object lifecycleLock = new Object();
    private final String threadName;
    private final int workerLimit;
    private final ExecutorService workers;
    private int activeLanes;
    private boolean closed;

    public CompletionDispatcher(String threadName) {
        this(threadName, Math.min(MAX_CONTEXT_WORKERS,
            Math.max(1, Runtime.getRuntime().availableProcessors())));
    }

    public CompletionDispatcher(String threadName, int maxWorkers) {
        this.threadName = Objects.requireNonNull(threadName, "threadName");
        if (maxWorkers <= 0) {
            throw new IllegalArgumentException("maxWorkers must be positive");
        }
        this.workerLimit = Math.min(maxWorkers,
            Math.max(1, Runtime.getRuntime().availableProcessors()));
        this.workers = Executors.newFixedThreadPool(workerLimit, runnable -> {
            Thread thread = new Thread(runnable, threadName);
            thread.setDaemon(true);
            return thread;
        });
    }

    public CompletionLane acquireLane() {
        synchronized (lifecycleLock) {
            if (closed) {
                throw new IllegalStateException(
                    "completion dispatcher is closed");
            }
            return new CompletionLane(this);
        }
    }

    public int workerLimit() {
        return workerLimit;
    }

    @Override
    public void close() {
        boolean shutdown;
        synchronized (lifecycleLock) {
            if (closed) {
                return;
            }
            closed = true;
            shutdown = activeLanes == 0;
        }
        if (shutdown) {
            workers.shutdown();
        }
    }

    private void laneActivated() {
        synchronized (lifecycleLock) {
            activeLanes++;
        }
    }

    private void laneIdle() {
        boolean shutdown;
        synchronized (lifecycleLock) {
            activeLanes--;
            shutdown = closed && activeLanes == 0;
        }
        if (shutdown) {
            workers.shutdown();
        }
    }

    private void schedule(Runnable runner) {
        try {
            workers.execute(runner);
        } catch (RejectedExecutionException rejected) {
            // Lifecycle races after shutdown must still hand work away from a
            // foreign Core callback. The lane runner retains FIFO ordering.
            Thread.ofVirtual()
                .name(threadName + "-teardown")
                .start(runner);
        }
    }

    /** One lightweight serial execution lane owned by a socket. */
    public static final class CompletionLane {
        private final CompletionDispatcher dispatcher;
        private final Deque<Runnable> queued = new ArrayDeque<>();
        private final Runnable runner = this::runOne;
        private boolean running;

        private CompletionLane(CompletionDispatcher dispatcher) {
            this.dispatcher = dispatcher;
        }

        public void dispatch(Runnable completion) {
            Objects.requireNonNull(completion, "completion");
            boolean schedule = false;
            synchronized (this) {
                queued.addLast(completion);
                if (!running) {
                    running = true;
                    dispatcher.laneActivated();
                    schedule = true;
                }
            }
            if (schedule) {
                dispatcher.schedule(runner);
            }
        }

        private void runOne() {
            Runnable completion;
            synchronized (this) {
                completion = queued.removeFirst();
            }
            try {
                completion.run();
            } finally {
                boolean scheduleNext;
                synchronized (this) {
                    scheduleNext = !queued.isEmpty();
                    if (!scheduleNext) {
                        running = false;
                    }
                }
                if (scheduleNext) {
                    dispatcher.schedule(runner);
                } else {
                    dispatcher.laneIdle();
                }
            }
        }
    }
}
