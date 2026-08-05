package systems.zlink.stream.connector;

import java.time.Instant;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.ThreadFactory;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;

final class ZLinkStreamInboundObserverDispatcher implements AutoCloseable {
    private final List<ZLinkStreamInboundObserver> observers = new CopyOnWriteArrayList<>();
    private final ArrayBlockingQueue<ZLinkStreamInboundObservation> queue;
    private final ExecutorService executor = Executors.newSingleThreadExecutor(
        new ObserverThreadFactory());
    private final Consumer<ZLinkStreamError> publishError;
    private final int maxPayloadPreviewBytes;
    private final AtomicBoolean dropReportPending = new AtomicBoolean();
    private volatile boolean closed;

    ZLinkStreamInboundObserverDispatcher(
        int maxNotifications,
        int maxPayloadPreviewBytes,
        Consumer<ZLinkStreamError> publishError) {
        this.queue = new ArrayBlockingQueue<>(maxNotifications);
        this.maxPayloadPreviewBytes = maxPayloadPreviewBytes;
        this.publishError = Objects.requireNonNull(publishError, "publishError");
        executor.execute(this::drainLoop);
    }

    AutoCloseable add(ZLinkStreamInboundObserver observer) {
        Objects.requireNonNull(observer, "observer");
        observers.add(observer);
        return () -> observers.remove(observer);
    }

    void enqueue(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        if (closed || observers.isEmpty()) {
            return;
        }
        ZLinkStreamInboundObservation observation = new ZLinkStreamInboundObservation(
            kindFromWire(header.kind()),
            header.name(),
            ZLinkStreamConnectorPayloadCodec.fromWireCodec(header.codec()),
            header.requestSeq(),
            header.metadata(),
            payload.length,
            (header.flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED) != 0,
            Instant.now(),
            Arrays.copyOf(payload, Math.min(payload.length, maxPayloadPreviewBytes)));
        if (!queue.offer(observation)) {
            reportDropped();
        }
    }

    private void drainLoop() {
        while (!closed || !queue.isEmpty()) {
            try {
                ZLinkStreamInboundObservation observation = queue.take();
                for (ZLinkStreamInboundObserver observer : List.copyOf(observers)) {
                    if (!observers.contains(observer)) {
                        continue;
                    }
                    invoke(observer, observation);
                }
            } catch (InterruptedException ex) {
                Thread.currentThread().interrupt();
                return;
            }
        }
    }

    private void invoke(
        ZLinkStreamInboundObserver observer,
        ZLinkStreamInboundObservation observation) {
        try {
            observer.observeAsync(observation).whenComplete((ignored, ex) -> {
                if (ex != null) {
                    reportObserverFailed(ex);
                }
            });
        } catch (Throwable ex) {
            reportObserverFailed(ex);
        }
    }

    private void reportObserverFailed(Throwable ex) {
        publishError.accept(new ZLinkStreamError(
            ZLinkStreamErrorCode.OBSERVER_FAILED,
            "Inbound observer callback failed.",
            ex));
    }

    private void reportDropped() {
        if (!dropReportPending.compareAndSet(false, true)) {
            return;
        }
        try {
            publishError.accept(new ZLinkStreamError(
                ZLinkStreamErrorCode.OBSERVER_DROPPED,
                "Inbound observer notification was dropped because the observer queue is full."));
        } finally {
            dropReportPending.set(false);
        }
    }

    private static ZLinkStreamMessageKind kindFromWire(int kind) {
        return switch (kind) {
            case ZLinkStreamWireProtocol.KIND_SEND -> ZLinkStreamMessageKind.SEND;
            case ZLinkStreamWireProtocol.KIND_REQUEST -> ZLinkStreamMessageKind.REQUEST;
            case ZLinkStreamWireProtocol.KIND_RESPONSE -> ZLinkStreamMessageKind.RESPONSE;
            case ZLinkStreamWireProtocol.KIND_ERROR -> ZLinkStreamMessageKind.ERROR;
            case ZLinkStreamWireProtocol.KIND_CONTROL -> ZLinkStreamMessageKind.CONTROL;
            default -> throw new IllegalArgumentException("unknown stream message kind");
        };
    }

    @Override
    public void close() {
        closed = true;
        executor.shutdownNow();
    }

    private static final class ObserverThreadFactory implements ThreadFactory {
        @Override
        public Thread newThread(Runnable task) {
            Thread thread = new Thread(task, "zlink-stream-inbound-observer");
            thread.setDaemon(true);
            return thread;
        }
    }
}
