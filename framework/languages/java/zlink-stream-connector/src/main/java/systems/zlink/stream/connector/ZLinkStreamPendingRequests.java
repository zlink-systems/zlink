package systems.zlink.stream.connector;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class ZLinkStreamPendingRequests {
    private final Map<Long, PendingRequest> requests =
        new ConcurrentHashMap<>();

    CompletableFuture<ZLinkStreamEncodedPayload> add(
        long requestSeq,
        String packetName,
        Duration timeout,
        ScheduledExecutorService scheduler) {
        CompletableFuture<ZLinkStreamEncodedPayload> pending = new CompletableFuture<>();
        requests.put(requestSeq, new PendingRequest(packetName, pending));
        var timeoutTask = scheduler.schedule(() -> {
            if (requests.remove(requestSeq) != null) {
                pending.completeExceptionally(
                    new TimeoutException("request timed out after " + timeout));
            }
        }, timeout.toMillis(), TimeUnit.MILLISECONDS);
        pending.whenComplete((reply, ex) -> {
            timeoutTask.cancel(false);
            if (pending.isCancelled()) {
                requests.remove(requestSeq);
            }
        });
        return pending;
    }

    void complete(long requestSeq, ZLinkStreamEncodedPayload payload) {
        PendingRequest request = requests.remove(requestSeq);
        CompletableFuture<ZLinkStreamEncodedPayload> pending = request == null ? null : request.future();
        if (pending == null) {
            payload.payload().close();
            return;
        }
        ZLinkStreamEncodedPayload reply = new ZLinkStreamEncodedPayload(
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
        CompletableFuture<ZLinkStreamEncodedPayload> pending = request == null ? null : request.future();
        if (pending != null) {
            pending.completeExceptionally(ex);
            return true;
        }
        return false;
    }

    void failAll(Throwable ex) {
        for (Map.Entry<Long, PendingRequest> entry
            : requests.entrySet()) {
            if (requests.remove(entry.getKey()) != null) {
                entry.getValue().future().completeExceptionally(ex);
            }
        }
    }

    private record PendingRequest(
        String packetName,
        CompletableFuture<ZLinkStreamEncodedPayload> future) {
    }
}
