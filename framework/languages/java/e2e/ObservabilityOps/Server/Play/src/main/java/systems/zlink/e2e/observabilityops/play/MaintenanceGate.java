package systems.zlink.e2e.observabilityops.play;

import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;

/** Holds one public Spot closing callback until the scenario releases it. */
public final class MaintenanceGate {
    private final AtomicReference<CompletableFuture<Void>> release =
        new AtomicReference<>(CompletableFuture.completedFuture(null));
    private volatile Instant callbackEnteredAt;
    private volatile Instant callbackDeadline;
    private volatile String callbackReason = "";

    public void arm() {
        release.set(new CompletableFuture<>());
        callbackEnteredAt = null;
        callbackDeadline = null;
        callbackReason = "";
    }

    public CompletionStage<Void> awaitClosing(ZLinkSpotClosingContext context) {
        callbackEnteredAt = Instant.now();
        callbackDeadline = context.deadline();
        callbackReason = context.reason().name();
        return release.get();
    }

    public void release() {
        release.get().complete(null);
    }

    public Map<String, Object> snapshot() {
        Map<String, Object> value = new LinkedHashMap<>();
        CompletableFuture<Void> current = release.get();
        value.put("armed", !current.isDone());
        value.put("callbackEnteredAt", instant(callbackEnteredAt));
        value.put("callbackDeadline", instant(callbackDeadline));
        value.put("callbackReason", callbackReason);
        return value;
    }

    private static String instant(Instant value) {
        return value == null ? "" : value.toString();
    }
}
