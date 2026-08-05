package systems.zlink.framework.execution;

import java.time.Duration;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executors;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Elastic bounded worker pool for CPU offload.
 *
 * <p>Semantics: threads are created on demand up to {@code maxThreads}, idle threads
 * above {@code minThreads} are reclaimed after {@code idleTimeout}, at most
 * {@code maxQueueLength} tasks wait in the queue, and a full queue rejects the submit
 * immediately ({@link RejectedExecutionException}) instead of blocking the caller.
 */
public final class ZLinkWorkerPool implements AutoCloseable {
    private static final AtomicInteger POOL_SEQUENCE = new AtomicInteger();

    private final ThreadPoolExecutor executor;
    private final ElasticTaskQueue queue;
    private final int minThreads;
    private final AtomicInteger inFlightTasks = new AtomicInteger();
    private final AtomicBoolean shutdownRequested = new AtomicBoolean();
    private final Set<Runnable> shutdownListeners = ConcurrentHashMap.newKeySet();
    private volatile ScheduledExecutorService timeoutScheduler;

    public ZLinkWorkerPool(
        int minThreads,
        int maxThreads,
        Duration idleTimeout,
        int maxQueueLength) {
        if (minThreads < 0) {
            throw new IllegalArgumentException("worker minThreads must be >= 0");
        }
        if (maxThreads < 1 || maxThreads < minThreads) {
            throw new IllegalArgumentException(
                "worker maxThreads must be >= 1 and >= minThreads");
        }
        if (idleTimeout == null || idleTimeout.isNegative()) {
            throw new IllegalArgumentException("worker idleTimeout must not be negative");
        }
        if (maxQueueLength < 1) {
            throw new IllegalArgumentException("worker maxQueueLength must be >= 1");
        }
        this.minThreads = minThreads;
        this.queue = new ElasticTaskQueue(maxQueueLength);
        int poolId = POOL_SEQUENCE.incrementAndGet();
        AtomicInteger threadSequence = new AtomicInteger();
        this.executor = new ThreadPoolExecutor(
            minThreads,
            maxThreads,
            Math.max(1, idleTimeout.toMillis()),
            TimeUnit.MILLISECONDS,
            queue,
            task -> {
                Thread thread = new Thread(
                    task,
                    "zlink-worker-" + poolId + "-" + threadSequence.incrementAndGet());
                thread.setDaemon(true);
                return thread;
            },
            this::queueWhenSaturated);
        queue.bind(executor, inFlightTasks);
    }

    public static int defaultMaxThreads() {
        return Math.max(2, Runtime.getRuntime().availableProcessors() * 2);
    }

    /**
     * Submits a task. Throws {@link RejectedExecutionException} immediately when the
     * pool is at {@code maxThreads} and the queue is full; never blocks the caller.
     */
    public void execute(Runnable task) {
        inFlightTasks.incrementAndGet();
        boolean submitted = false;
        try {
            executor.execute(() -> {
                try {
                    task.run();
                } finally {
                    inFlightTasks.decrementAndGet();
                }
            });
            submitted = true;
        } finally {
            if (!submitted) {
                inFlightTasks.decrementAndGet();
            }
        }
    }

    /** Schedules a timeout action on a shared scheduler owned by this pool. */
    public ScheduledFuture<?> scheduleTimeout(Runnable action, Duration delay) {
        ScheduledExecutorService scheduler = timeoutScheduler;
        if (scheduler == null) {
            synchronized (this) {
                scheduler = timeoutScheduler;
                if (scheduler == null) {
                    scheduler = Executors.newSingleThreadScheduledExecutor(task -> {
                        Thread thread = new Thread(task, "zlink-worker-timeout");
                        thread.setDaemon(true);
                        return thread;
                    });
                    timeoutScheduler = scheduler;
                }
            }
        }
        return scheduler.schedule(action, delay.toNanos(), TimeUnit.NANOSECONDS);
    }

    /** Number of live worker threads; used by idle-shrink diagnostics and tests. */
    public int poolSize() {
        return executor.getPoolSize();
    }

    public int queueLength() {
        return queue.size();
    }

    /**
     * Registers work that must be notified when shutdown starts.
     *
     * @return an idempotent action that removes the listener
     */
    public Runnable registerShutdownListener(Runnable listener) {
        if (shutdownRequested.get()) {
            listener.run();
            return () -> { };
        }
        shutdownListeners.add(listener);
        if (shutdownRequested.get() && shutdownListeners.remove(listener)) {
            listener.run();
        }
        return () -> shutdownListeners.remove(listener);
    }

    @Override
    public void close() {
        if (shutdownRequested.compareAndSet(false, true)) {
            shutdownListeners.forEach(Runnable::run);
            shutdownListeners.clear();
        }
        executor.shutdownNow();
        ScheduledExecutorService scheduler = timeoutScheduler;
        if (scheduler != null) {
            scheduler.shutdownNow();
        }
    }

    private void queueWhenSaturated(Runnable task, ThreadPoolExecutor pool) {
        if (pool.isShutdown()) {
            throw new RejectedExecutionException("worker pool is closed");
        }
        // The elastic queue refused the offer to force thread creation, but the pool
        // is already at maxThreads. Try to queue the task directly; a full queue is a
        // hard, immediate rejection (no waiting, no caller-runs).
        if (!queue.offerDirect(task)) {
            throw new RejectedExecutionException("worker queue is full");
        }
        ensureQueuedTaskHasWorker(pool);
    }

    private void ensureQueuedTaskHasWorker(ThreadPoolExecutor pool) {
        // Close the shutdown/idle race where the task was queued after the last
        // worker passed its exit check: briefly raise the core size to spawn one
        // worker, then restore the elastic minimum.
        if (pool.getPoolSize() == 0) {
            synchronized (this) {
                if (pool.getPoolSize() == 0 && !pool.isShutdown()) {
                    pool.setCorePoolSize(1);
                    pool.prestartCoreThread();
                    pool.setCorePoolSize(minThreads);
                }
            }
        }
    }

    /**
     * Bounded queue that reports itself full while the pool can still grow, so the
     * executor creates threads before queueing (elastic scale-out).
     */
    private static final class ElasticTaskQueue extends LinkedBlockingQueue<Runnable> {
        private transient volatile ThreadPoolExecutor pool;
        private transient volatile AtomicInteger inFlightTasks;

        ElasticTaskQueue(int capacity) {
            super(capacity);
        }

        void bind(ThreadPoolExecutor pool, AtomicInteger inFlightTasks) {
            this.pool = pool;
            this.inFlightTasks = inFlightTasks;
        }

        boolean offerDirect(Runnable task) {
            return super.offer(task);
        }

        @Override
        public boolean offer(Runnable task) {
            ThreadPoolExecutor bound = pool;
            AtomicInteger inFlight = inFlightTasks;
            if (bound == null || inFlight == null) {
                return super.offer(task);
            }
            int poolSize = bound.getPoolSize();
            if (poolSize >= bound.getMaximumPoolSize()) {
                return super.offer(task);
            }
            // Reuse an idle thread when one exists; otherwise refuse the offer so the
            // executor grows the pool toward maxThreads before queueing.
            if (inFlight.get() <= poolSize) {
                return super.offer(task);
            }
            return false;
        }
    }
}
