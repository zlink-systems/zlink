package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.calls.ZLinkBlockingCalls;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;

final class ZLinkChannelReceiveLoops implements AutoCloseable {
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);
    private static final long NO_RECORD = -1L;
    private final BooleanSupplier running;
    private final ZLinkApplicationJobQueue applicationJobQueue;
    private volatile boolean closed;
    private final ConcurrentLinkedQueue<ReceiveLoop> receiveLoops = new ConcurrentLinkedQueue<>();
    private final ExecutorService executor =
            Executors.newCachedThreadPool(
                    task -> {
                        Thread thread =
                                new Thread(
                                        ZLinkBlockingCalls.infrastructureTask(task),
                                        "zlink-java-channel-runtime");
                        thread.setDaemon(true);
                        return thread;
                    });

    ZLinkChannelReceiveLoops(
            BooleanSupplier running, ZLinkApplicationJobQueue applicationJobQueue) {
        this.running = running;
        this.applicationJobQueue =
                java.util.Objects.requireNonNull(applicationJobQueue, "applicationJobQueue");
    }

    void startRequest(
            ZLinkBackendRouterSocket router,
            Consumer<ZLinkBackendReceived> dispatch,
            Consumer<Throwable> reportFailure) {
        start(
                new ReceiveLoop(reportFailure) {
                    @Override
                    boolean waitForReadable() {
                        return router.waitForReadable(RECEIVE_POLL_TIMEOUT);
                    }

                    @Override
                    long receiveOne() {
                        assertReceiveOwner();
                        ZLinkBackendReceived received = router.recv(ZLinkBackendRecvMode.DONT_WAIT);
                        if (received == null) {
                            return NO_RECORD;
                        }
                        long bytes =
                                ZLinkReceiveBatchBudget.bytesOf(
                                        received.parts(),
                                        received.applicationMetadataSize(),
                                        received.acceptedJournalRecordSize());
                        dispatch.accept(received);
                        return bytes;
                    }
                });
    }

    void startSubscribe(
            ZLinkBackendSubscriberSocket subscriber,
            Consumer<ZLinkBackendTopicMessage> dispatch,
            Consumer<Throwable> reportFailure) {
        start(
                new ReceiveLoop(reportFailure) {
                    @Override
                    boolean waitForReadable() {
                        return subscriber.waitForReadable(RECEIVE_POLL_TIMEOUT);
                    }

                    @Override
                    long receiveOne() {
                        assertReceiveOwner();
                        ZLinkBackendTopicMessage received =
                                subscriber.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
                        if (received == null) {
                            return NO_RECORD;
                        }
                        long bytes =
                                ZLinkReceiveBatchBudget.bytesOf(
                                        received.parts(),
                                        received.applicationMetadataSize(),
                                        utf8Length(received.topic()));
                        dispatch.accept(received);
                        return bytes;
                    }
                });
    }

    void startRoute(
            ZLinkBackendRouterSocket router,
            Object socketLock,
            Runnable drainBridge,
            Consumer<ZLinkBackendReceived> dispatch,
            Consumer<Throwable> reportFailure) {
        start(
                new ReceiveLoop(reportFailure) {
                    @Override
                    boolean waitForReadable() {
                        return router.waitForReadable(RECEIVE_POLL_TIMEOUT);
                    }

                    @Override
                    void batchBoundary() {
                        drainBridge.run();
                    }

                    @Override
                    long receiveOne() {
                        ZLinkBackendReceived received;
                        synchronized (socketLock) {
                            assertReceiveOwner();
                            received = router.recv(ZLinkBackendRecvMode.DONT_WAIT);
                        }
                        if (received == null) {
                            return NO_RECORD;
                        }
                        long bytes =
                                ZLinkReceiveBatchBudget.bytesOf(
                                        received.parts(),
                                        received.applicationMetadataSize(),
                                        received.acceptedJournalRecordSize());
                        dispatch.accept(received);
                        return bytes;
                    }
                });
    }

    @Override
    public void close() {
        closed = true;
        receiveLoops.forEach(
                loop -> ZLinkApplicationJobQueue.cancelPendingAcquire(loop.pendingAcquire));
        executor.shutdownNow();
    }

    void awaitTermination() {
        boolean interrupted = false;
        while (!executor.isTerminated()) {
            try {
                executor.awaitTermination(
                        Long.MAX_VALUE, java.util.concurrent.TimeUnit.NANOSECONDS);
            } catch (InterruptedException interruption) {
                interrupted = true;
                executor.shutdownNow();
            }
        }
        if (interrupted) {
            Thread.currentThread().interrupt();
        }
    }

    private void start(ReceiveLoop loop) {
        receiveLoops.add(loop);
        executor.execute(loop);
    }

    /**
     * One socket's receive owner. It runs as a sequence of turns on {@link #executor}; exactly one
     * turn runs at a time. A turn ends when no permit is free: the Application Job Queue grant
     * starts the next turn with that permit, so no thread waits for capacity.
     */
    private abstract class ReceiveLoop implements Runnable {
        private final Consumer<Throwable> reportFailure;
        private final AtomicReference<CompletableFuture<ZLinkApplicationJobQueue.Permit>>
                pendingAcquire = new AtomicReference<>();
        private Thread ownerThread;

        private ReceiveLoop(Consumer<Throwable> reportFailure) {
            this.reportFailure = reportFailure;
        }

        @Override
        public final void run() {
            if (ownerThread != null) {
                throw new IllegalStateException("socket receive loop already has an owner thread");
            }
            runTurn(null);
        }

        private void runTurn(ZLinkApplicationJobQueue.Permit granted) {
            // The previous turn handed ownership over through the queue grant and the executor,
            // and it touches nothing after it returned the permit wait to the queue.
            ownerThread = Thread.currentThread();
            ZLinkApplicationJobQueue.Permit first = granted;
            try {
                while (running.getAsBoolean() && !closed) {
                    try {
                        if (first == null && !waitForReadable()) {
                            if (Thread.currentThread().isInterrupted()) {
                                return;
                            }
                            continue;
                        }
                        ZLinkApplicationJobQueue.Permit batchFirst = first;
                        first = null;
                        if (!receiveBatch(batchFirst)) {
                            return;
                        }
                    } catch (RuntimeException error) {
                        if (ZLinkChannelRuntime.isNoDataReceive(error)) {
                            continue;
                        }
                        reportFailure.accept(error);
                    }
                }
            } finally {
                if (first != null) {
                    first.abandonReservation();
                }
            }
        }

        /** Returns false when this turn ended: the permit wait or a closed queue owns the loop. */
        private boolean receiveBatch(ZLinkApplicationJobQueue.Permit first) {
            ZLinkApplicationJobQueue.Permit permit = first;
            try {
                batchBoundary();
            } catch (RuntimeException | Error failure) {
                if (permit != null) {
                    permit.abandonReservation();
                }
                throw failure;
            }
            ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
            while (batch.canReceiveNext()) {
                if (permit == null) {
                    permit = reserveBeforeReceive(this);
                    if (permit == null) {
                        return false;
                    }
                }
                try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                    long bytes = receiveOne();
                    if (bytes == NO_RECORD) {
                        break;
                    }
                    batch.record(bytes);
                } finally {
                    permit.abandonReservation();
                    permit = null;
                }
            }
            batchBoundary();
            return true;
        }

        final void assertReceiveOwner() {
            if (Thread.currentThread() != ownerThread) {
                throw new IllegalStateException(
                        "socket receive must run on its receive-loop owner thread");
            }
        }

        abstract boolean waitForReadable();

        /** Receives and dispatches one record, returning its budget bytes or NO_RECORD. */
        abstract long receiveOne();

        void batchBoundary() {}
    }

    private ZLinkApplicationJobQueue.Permit reserveBeforeReceive(ReceiveLoop loop) {
        ZLinkApplicationJobQueue.Permit permit =
                applicationJobQueue.acquireOrResume(executor, loop::runTurn, loop.pendingAcquire);
        if (closed) {
            ZLinkApplicationJobQueue.cancelPendingAcquire(loop.pendingAcquire);
            if (permit != null) {
                permit.abandonReservation();
            }
            return null;
        }
        return permit;
    }

    static int utf8Length(String value) {
        int length = 0;
        for (int i = 0; i < value.length(); i++) {
            char current = value.charAt(i);
            if (current < 0x80) {
                length++;
            } else if (current < 0x800) {
                length += 2;
            } else if (Character.isHighSurrogate(current)
                    && i + 1 < value.length()
                    && Character.isLowSurrogate(value.charAt(i + 1))) {
                length += 4;
                i++;
            } else if (Character.isSurrogate(current)) {
                length++;
            } else {
                length += 3;
            }
        }
        return length;
    }
}
