package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;

/** Coordinates application-level gates used by process E2E scenarios. */
public final class GateController {
    private final ConcurrentMap<String, CompletableFuture<Void>> closed =
        new ConcurrentHashMap<>();

    public void set(String gateId, boolean open) {
        if (open) {
            CompletableFuture<Void> gate = closed.remove(gateId);
            if (gate != null) {
                gate.complete(null);
            }
        } else {
            closed.computeIfAbsent(gateId, ignored -> new CompletableFuture<>());
        }
    }

    public CompletionStage<Void> await(String gateId) {
        if (gateId == null || gateId.isBlank()) {
            return CompletableFuture.completedFuture(null);
        }
        CompletableFuture<Void> gate = closed.get(gateId);
        return gate == null ? CompletableFuture.completedFuture(null) : gate;
    }

    public CompletionStage<Void> awaitPayload(String payload) {
        if (payload == null || !payload.startsWith("gate:")) {
            return CompletableFuture.completedFuture(null);
        }
        int separator = payload.indexOf('|');
        String gateId = separator < 0
            ? payload.substring("gate:".length())
            : payload.substring("gate:".length(), separator);
        return await(gateId);
    }
}
