package systems.zlink.framework.runtime.channels;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.BooleanSupplier;
import java.util.function.Consumer;
import java.util.function.Supplier;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

final class ZLinkChannelReceiveLoops implements AutoCloseable {
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);
    private final BooleanSupplier running;
    private final ZLinkApplicationJobQueue applicationJobQueue;
    private final Object capacityLock = new Object();
    private boolean closed;
    private final ExecutorService executor =
        Executors.newCachedThreadPool(task -> {
            Thread thread = new Thread(task, "zlink-java-channel-runtime");
            thread.setDaemon(true);
            return thread;
        });

    ZLinkChannelReceiveLoops(
        BooleanSupplier running,
        ZLinkApplicationJobQueue applicationJobQueue) {
        this.running = running;
        this.applicationJobQueue = java.util.Objects.requireNonNull(
            applicationJobQueue, "applicationJobQueue");
    }

    void startRequest(
        ZLinkBackendRouterSocket router,
        Consumer<ZLinkBackendReceived> dispatch,
        Consumer<Throwable> reportFailure) {
        start(new ReceiveLoop(reportFailure) {
            @Override
            boolean receiveAndDispatch() {
                if (!router.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
                    return false;
                }
                ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
                boolean dispatched = false;
                while (batch.canReceiveNext()) {
                    ZLinkApplicationJobQueue.Permit permit = reserveBeforeReceive();
                    if (permit == null) {
                        break;
                    }
                    try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                        ZLinkBackendReceived received = router.recv(
                            ZLinkBackendRecvMode.DONT_WAIT);
                        if (received == null) {
                            break;
                        }
                        batch.record(ZLinkReceiveBatchBudget.bytesOf(
                            received.parts(),
                            received.applicationMetadataSize(),
                            received.acceptedJournalRecordSize()));
                        dispatch.accept(received);
                        dispatched = true;
                    } finally {
                        permit.abandonReservation();
                    }
                }
                return dispatched;
            }
        });
    }

    void startSubscribe(
        ZLinkBackendSubscriberSocket subscriber,
        Consumer<ZLinkBackendTopicMessage> dispatch,
        Consumer<Throwable> reportFailure) {
        start(new ReceiveLoop(reportFailure) {
            @Override
            boolean receiveAndDispatch() {
                if (!subscriber.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
                    return false;
                }
                ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
                boolean dispatched = false;
                while (batch.canReceiveNext()) {
                    ZLinkApplicationJobQueue.Permit permit = reserveBeforeReceive();
                    if (permit == null) {
                        break;
                    }
                    try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                        ZLinkBackendTopicMessage received = subscriber.subscribe(
                            ZLinkBackendRecvMode.DONT_WAIT);
                        if (received == null) {
                            break;
                        }
                        batch.record(ZLinkReceiveBatchBudget.bytesOf(
                            received.parts(),
                            received.applicationMetadataSize(),
                            received.topic().getBytes(StandardCharsets.UTF_8).length));
                        dispatch.accept(received);
                        dispatched = true;
                    } finally {
                        permit.abandonReservation();
                    }
                }
                return dispatched;
            }
        });
    }

    void startRoute(
        ZLinkBackendRouterSocket router,
        Supplier<Object> socketLock,
        Runnable drainBridge,
        Consumer<ZLinkBackendReceived> dispatch,
        Consumer<Throwable> reportFailure) {
        start(new ReceiveLoop(reportFailure) {
            @Override
            boolean receiveAndDispatch() {
                if (!router.waitForReadable(RECEIVE_POLL_TIMEOUT)) {
                    return false;
                }
                ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
                boolean dispatched = false;
                drainBridge.run();
                while (batch.canReceiveNext()) {
                    ZLinkApplicationJobQueue.Permit permit = reserveBeforeReceive();
                    if (permit == null) {
                        break;
                    }
                    try (var ignored = ZLinkApplicationJobContext.enter(permit)) {
                        ZLinkBackendReceived received;
                        synchronized (socketLock.get()) {
                            received = router.recv(ZLinkBackendRecvMode.DONT_WAIT);
                        }
                        if (received == null) {
                            break;
                        }
                        batch.record(ZLinkReceiveBatchBudget.bytesOf(
                            received.parts(),
                            received.applicationMetadataSize(),
                            received.acceptedJournalRecordSize()));
                        dispatch.accept(received);
                        dispatched = true;
                    } finally {
                        permit.abandonReservation();
                    }
                }
                drainBridge.run();
                return dispatched;
            }
        });
    }

    @Override
    public void close() {
        synchronized (capacityLock) {
            closed = true;
            capacityLock.notifyAll();
        }
        executor.shutdownNow();
    }

    void awaitTermination() {
        ZLinkChannelRuntime.awaitTerminated(executor);
    }

    private void start(ReceiveLoop loop) {
        executor.execute(loop);
    }

    private abstract class ReceiveLoop implements Runnable {
        private final Consumer<Throwable> reportFailure;

        private ReceiveLoop(Consumer<Throwable> reportFailure) {
            this.reportFailure = reportFailure;
        }

        @Override
        public final void run() {
            while (running.getAsBoolean() && !isClosed()) {
                try {
                    if (!receiveAndDispatch()) {
                        if (Thread.currentThread().isInterrupted()) {
                            return;
                        }
                        awaitCapacity();
                    }
                } catch (RuntimeException error) {
                    if (ZLinkChannelRuntime.isNoDataReceive(error)) {
                        continue;
                    }
                    reportFailure.accept(error);
                }
            }
        }

        abstract boolean receiveAndDispatch();
    }

    private void awaitCapacity() {
        synchronized (capacityLock) {
            // Socket poll timeout handles idle receive without a Framework pause waiter.
        }
    }

    private boolean isClosed() {
        synchronized (capacityLock) {
            return closed;
        }
    }

    private ZLinkApplicationJobQueue.Permit reserveBeforeReceive() {
        try {
            return applicationJobQueue.acquireBlocking();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return null;
        } catch (java.util.concurrent.CancellationException closedQueue) {
            if (isClosed()) {
                return null;
            }
            throw closedQueue;
        }
    }
}
