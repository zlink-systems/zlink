package systems.zlink.e2e.spotactortransfer.actor;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;

public final class GateStore {
    private final ConcurrentHashMap<String, CompletableFuture<Void>> gates = new ConcurrentHashMap<>();

    public CompletableFuture<Void> waitFor(String key) {
        return gates.computeIfAbsent(key, ignored -> new CompletableFuture<>());
    }

    public boolean release(String key) {
        return gates.computeIfAbsent(key, ignored -> new CompletableFuture<>()).complete(null);
    }
}
