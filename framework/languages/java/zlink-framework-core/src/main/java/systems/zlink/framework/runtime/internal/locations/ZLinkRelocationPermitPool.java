package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import systems.zlink.framework.locations.ZLinkLocationOptions;

/**
 * Accounts for every process-wide relocation resource as one admission unit.
 * A failed acquisition leaves every counter unchanged.
 */
public final class ZLinkRelocationPermitPool {
    private final int maxOutboundUnits;
    private final int maxInboundUnits;
    private final int maxCaptureCallbacks;
    private final int maxRestoreCallbacks;
    private final long maxPayloadBytes;
    private int outboundUnits;
    private int inboundUnits;
    private int captureCallbacks;
    private int restoreCallbacks;
    private long payloadBytes;
    private boolean oversizedPayloadActive;

    public ZLinkRelocationPermitPool(ZLinkLocationOptions options) {
        Objects.requireNonNull(options, "options");
        maxOutboundUnits = options.maxActiveOutboundRelocations();
        maxInboundUnits = options.maxActiveInboundRelocations();
        maxCaptureCallbacks = options.maxConcurrentRelocationCaptures();
        maxRestoreCallbacks = options.maxConcurrentRelocationRestores();
        maxPayloadBytes = options.maxRelocationPayloadInFlightBytes();
    }

    public synchronized Lease tryAcquire(Request request) {
        Objects.requireNonNull(request, "request");
        boolean oversized = request.payloadBytes() > maxPayloadBytes;
        if (outboundUnits > maxOutboundUnits - request.outboundUnits()
            || inboundUnits > maxInboundUnits - request.inboundUnits()
            || captureCallbacks > maxCaptureCallbacks - request.captureCallbacks()
            || restoreCallbacks > maxRestoreCallbacks - request.restoreCallbacks()
            || !canAdmitPayload(request, oversized)) {
            return null;
        }
        outboundUnits += request.outboundUnits();
        inboundUnits += request.inboundUnits();
        captureCallbacks += request.captureCallbacks();
        restoreCallbacks += request.restoreCallbacks();
        payloadBytes = Math.addExact(payloadBytes, request.payloadBytes());
        oversizedPayloadActive = oversized;
        return new Lease(this, request, oversized);
    }

    public CompletionStage<Lease> acquire(
        Request request,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(request, "request");
        Objects.requireNonNull(cancellation, "cancellation");
        Lease acquired = tryAcquire(request);
        if (acquired != null) {
            return CompletableFuture.completedFuture(acquired);
        }
        if (cancellation.isCancellationRequested()) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "relocation permit acquisition was cancelled"));
        }
        return CompletableFuture.runAsync(
                () -> {
                },
                CompletableFuture.delayedExecutor(
                    5, TimeUnit.MILLISECONDS))
            .thenCompose(ignored -> acquire(request, cancellation));
    }

    public synchronized Snapshot snapshot() {
        return new Snapshot(
            outboundUnits,
            inboundUnits,
            captureCallbacks,
            restoreCallbacks,
            payloadBytes,
            oversizedPayloadActive);
    }

    private boolean canAdmitPayload(Request request, boolean oversized) {
        if (oversized) {
            return request.allowOversizedPayload()
                && !oversizedPayloadActive
                && payloadBytes == 0;
        }
        return !oversizedPayloadActive
            && payloadBytes <= maxPayloadBytes - request.payloadBytes();
    }

    private synchronized boolean shrink(Lease lease, long actualPayloadBytes) {
        if (actualPayloadBytes < 0) {
            throw new IllegalArgumentException(
                "actualPayloadBytes must not be negative");
        }
        if (lease.closed || actualPayloadBytes > lease.request.payloadBytes()) {
            return false;
        }
        payloadBytes -= lease.request.payloadBytes() - actualPayloadBytes;
        lease.request = lease.request.withPayloadBytes(actualPayloadBytes);
        return true;
    }

    private synchronized void release(Lease lease) {
        if (lease.closed) {
            return;
        }
        lease.closed = true;
        Request request = lease.request;
        outboundUnits -= request.outboundUnits();
        inboundUnits -= request.inboundUnits();
        captureCallbacks -= request.captureCallbacks();
        restoreCallbacks -= request.restoreCallbacks();
        payloadBytes -= request.payloadBytes();
        if (lease.oversized) {
            oversizedPayloadActive = false;
        }
        if (outboundUnits < 0 || inboundUnits < 0
            || captureCallbacks < 0 || restoreCallbacks < 0
            || payloadBytes < 0) {
            throw new IllegalStateException(
                "relocation permit accounting became negative");
        }
    }

    public record Request(
        int outboundUnits,
        int inboundUnits,
        int captureCallbacks,
        int restoreCallbacks,
        long payloadBytes,
        boolean allowOversizedPayload) {
        public Request {
            if (outboundUnits < 0 || inboundUnits < 0
                || captureCallbacks < 0 || restoreCallbacks < 0
                || payloadBytes < 0) {
                throw new IllegalArgumentException(
                    "relocation permit values must not be negative");
            }
            if (outboundUnits == 0 && inboundUnits == 0
                && captureCallbacks == 0 && restoreCallbacks == 0
                && payloadBytes == 0) {
                throw new IllegalArgumentException(
                    "a relocation permit must reserve at least one resource");
            }
        }

        public static Request outbound(long bytes, boolean capture) {
            return new Request(1, 0, capture ? 1 : 0, 0, bytes, false);
        }

        public static Request outboundAggregate(
            long bytes,
            int captureCallbacks,
            boolean allowOversizedPayload) {
            return new Request(
                1,
                0,
                captureCallbacks,
                0,
                bytes,
                allowOversizedPayload);
        }

        public static Request inbound(long bytes, boolean restore) {
            return inbound(bytes, restore, false);
        }

        public static Request inbound(
            long bytes,
            boolean restore,
            boolean allowOversizedPayload) {
            return new Request(
                0,
                1,
                0,
                restore ? 1 : 0,
                bytes,
                allowOversizedPayload);
        }

        Request withPayloadBytes(long bytes) {
            return new Request(
                outboundUnits,
                inboundUnits,
                captureCallbacks,
                restoreCallbacks,
                bytes,
                allowOversizedPayload);
        }
    }

    public record Snapshot(
        int outboundUnits,
        int inboundUnits,
        int captureCallbacks,
        int restoreCallbacks,
        long payloadBytes,
        boolean oversizedPayloadActive) {
    }

    public static final class Lease implements AutoCloseable {
        private final ZLinkRelocationPermitPool owner;
        private final boolean oversized;
        private Request request;
        private boolean closed;

        private Lease(
            ZLinkRelocationPermitPool owner,
            Request request,
            boolean oversized) {
            this.owner = owner;
            this.request = request;
            this.oversized = oversized;
        }

        public synchronized long reservedPayloadBytes() {
            return closed ? 0 : request.payloadBytes();
        }

        public synchronized boolean tryShrinkPayload(long actualPayloadBytes) {
            return owner.shrink(this, actualPayloadBytes);
        }

        @Override
        public synchronized void close() {
            owner.release(this);
        }
    }
}
