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
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

final class ZLinkChannelReceiveLoops implements AutoCloseable {
    private static final Duration RECEIVE_POLL_TIMEOUT = Duration.ofMillis(250);
    private final BooleanSupplier running;
    private final ZLinkInboundDispatchBudget inboundDispatchBudget;
    private final Object capacityLock = new Object();
    private final AutoCloseable capacityRegistration;
    private boolean capacitySignal;
    private boolean closed;
    private final ExecutorService executor =
        Executors.newCachedThreadPool(task -> {
            Thread thread = new Thread(task, "zlink-java-channel-runtime");
            thread.setDaemon(true);
            return thread;
        });

    ZLinkChannelReceiveLoops(BooleanSupplier running) {
        this(running, new ZLinkInboundDispatchBudget(0));
    }

    ZLinkChannelReceiveLoops(
        BooleanSupplier running,
        ZLinkInboundDispatchBudget inboundDispatchBudget) {
        this.running = running;
        this.inboundDispatchBudget = inboundDispatchBudget;
        this.capacityRegistration = inboundDispatchBudget.onCapacityAvailable(
            this::signalCapacity);
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
                while (batch.canReceiveNext()
                    && inboundDispatchBudget.canStartApplicationReceive()) {
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
                while (batch.canReceiveNext()
                    && inboundDispatchBudget.canStartApplicationReceive()) {
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
                while (batch.canReceiveNext()
                    && inboundDispatchBudget.canStartApplicationReceive()) {
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
            capacitySignal = true;
            capacityLock.notifyAll();
        }
        try {
            capacityRegistration.close();
        } catch (Exception ignored) {
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

    private void signalCapacity() {
        synchronized (capacityLock) {
            capacitySignal = true;
            capacityLock.notifyAll();
        }
    }

    private void awaitCapacity() {
        synchronized (capacityLock) {
            while (!closed
                && running.getAsBoolean()
                && !inboundDispatchBudget.canStartApplicationReceive()) {
                if (capacitySignal) {
                    capacitySignal = false;
                    continue;
                }
                try {
                    capacityLock.wait();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
            capacitySignal = false;
        }
    }

    private boolean isClosed() {
        synchronized (capacityLock) {
            return closed;
        }
    }
}
