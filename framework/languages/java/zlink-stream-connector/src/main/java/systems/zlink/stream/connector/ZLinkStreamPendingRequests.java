package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;

final class ZLinkStreamPendingRequests {
    private final Map<Long, PendingRequest> requests = new ConcurrentHashMap<>();

    CompletableFuture<ZLinkStreamEncodedPayload> add(long requestSeq, String packetName) {
        CompletableFuture<ZLinkStreamEncodedPayload> pending = new CompletableFuture<>();
        requests.put(requestSeq, new PendingRequest(packetName, pending));
        pending.whenComplete(
                (reply, ex) -> {
                    if (pending.isCancelled()) {
                        requests.remove(requestSeq);
                    }
                });
        return pending;
    }

    void startTimeout(long requestSeq, Duration timeout, ScheduledExecutorService scheduler) {
        PendingRequest request = requests.get(requestSeq);
        if (request == null) {
            return;
        }
        CompletableFuture<ZLinkStreamEncodedPayload> pending = request.future();
        var timeoutTask =
                scheduler.schedule(
                        () -> {
                            if (requests.remove(requestSeq, request)) {
                                pending.completeExceptionally(
                                        ZLinkStreamException.of(
                                                ZLinkStreamErrorCode.REQUEST_TIMEOUT,
                                                "request timed out after " + timeout,
                                                new TimeoutException(
                                                        "request timed out after " + timeout)));
                            }
                        },
                        timeout.toMillis(),
                        TimeUnit.MILLISECONDS);
        pending.whenComplete(
                (reply, ex) -> {
                    timeoutTask.cancel(false);
                });
    }

    void complete(long requestSeq, Supplier<ZLinkStreamEncodedPayload> decode) {
        PendingRequest request = requests.get(requestSeq);
        if (request == null) {
            return;
        }
        ZLinkStreamEncodedPayload payload = decode.get();
        if (!requests.remove(requestSeq, request)) {
            payload.payload().close();
            return;
        }
        CompletableFuture<ZLinkStreamEncodedPayload> pending = request.future();
        ZLinkStreamEncodedPayload reply =
                new ZLinkStreamEncodedPayload(
                        request.packetName(),
                        payload.payload(),
                        payload.metadata(),
                        payload.codec());
        if (!pending.complete(reply)) {
            reply.payload().close();
        }
    }

    boolean fail(long requestSeq, Throwable ex) {
        PendingRequest request = requests.remove(requestSeq);
        CompletableFuture<ZLinkStreamEncodedPayload> pending =
                request == null ? null : request.future();
        if (pending != null) {
            pending.completeExceptionally(ex);
            return true;
        }
        return false;
    }

    /**
     * Fails every pending request because the connection ended. Spec 32 9: they fail with {@code
     * Disconnected} whatever ended the connection; the cause stays in the close reason and in the
     * exception's cause.
     */
    void failAll(Throwable ex) {
        ZLinkStreamException failure =
                ex instanceof ZLinkStreamException coded
                                && coded.errorCode() == ZLinkStreamErrorCode.DISCONNECTED
                        ? coded
                        : ZLinkStreamException.of(
                                ZLinkStreamErrorCode.DISCONNECTED,
                                "connection ended: " + coded(ex).getMessage(),
                                ex);
        for (Map.Entry<Long, PendingRequest> entry : requests.entrySet()) {
            if (requests.remove(entry.getKey()) != null) {
                entry.getValue().future().completeExceptionally(failure);
            }
        }
    }

    static ZLinkStreamException coded(Throwable ex) {
        if (ex instanceof ZLinkStreamException coded) {
            return coded;
        }
        String message =
                ex.getMessage() == null || ex.getMessage().isBlank()
                        ? ex.getClass().getSimpleName()
                        : ex.getMessage();
        return ZLinkStreamException.of(ZLinkStreamErrorCode.DISCONNECTED, message, ex);
    }

    private record PendingRequest(
            String packetName, CompletableFuture<ZLinkStreamEncodedPayload> future) {}
}
