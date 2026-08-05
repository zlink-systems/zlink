package systems.zlink.framework.runtime.binding;

import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.BiFunction;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;

/**
 * Framework-owned Instance Spot activation barrier. Only an Instance-marked
 * direct operation calls this registry; ordinary missing Spot routes do not.
 */
final class ZLinkJavaInstanceSpotRegistry {
    private final Map<String, BiFunction<String, Long, ZLinkBackendSpot>>
        factories =
        new ConcurrentHashMap<>();
    private final Map<String, ActivationHook> hooks = new ConcurrentHashMap<>();
    private final Map<String, String> stableTypes =
        new ConcurrentHashMap<>();
    private final Map<String, CompletableFuture<Activation>> activations =
        new ConcurrentHashMap<>();

    void register(
        String stableType,
        BiFunction<String, Long, ZLinkBackendSpot> factory) {
        register(
            stableType,
            factory,
            (type, spotId, generation, spot) ->
                CompletableFuture.completedFuture(null));
    }

    void register(
        String stableType,
        BiFunction<String, Long, ZLinkBackendSpot> factory,
        ActivationHook hook) {
        requireType(stableType);
        if (factories.putIfAbsent(
            stableType,
            Objects.requireNonNull(factory, "factory")) != null) {
            throw new IllegalStateException(
                "Instance Spot type is already registered: " + stableType);
        }
        hooks.put(stableType, Objects.requireNonNull(hook, "hook"));
    }

    CompletionStage<Activation> activate(
        String spotId,
        String requestedType,
        long objectGeneration) {
        Objects.requireNonNull(spotId, "spotId");
        if (objectGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "Instance Spot object generation must be positive"));
        }
        String selected = selectType(spotId, requestedType);
        String recorded = stableTypes.putIfAbsent(spotId, selected);
        if (recorded != null && !recorded.equals(selected)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "Instance Spot stable type does not match authority"));
        }
        CompletableFuture<Activation> current = activations.get(spotId);
        if (current != null) {
            return current.thenCompose(activation ->
                activation.spot().lifecycleGeneration() == objectGeneration
                    ? CompletableFuture.completedFuture(activation)
                    : CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Instance Spot object generation is stale")));
        }
        CompletableFuture<Activation> candidate = new CompletableFuture<>();
        current = activations.putIfAbsent(spotId, candidate);
        if (current != null) {
            return current.thenCompose(activation ->
                activation.spot().lifecycleGeneration() == objectGeneration
                    ? CompletableFuture.completedFuture(activation)
                    : CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "Instance Spot object generation is stale")));
        }
        try {
            ZLinkBackendSpot spot =
                factories.get(selected).apply(spotId, objectGeneration);
            if (spot == null) {
                throw new IllegalStateException(
                    "Instance Spot factory returned null");
            }
            if (spot.lifecycleGeneration() != objectGeneration) {
                throw new IllegalStateException(
                    "Instance Spot factory returned a stale generation");
            }
            hooks.get(selected).activate(
                    selected, spotId, objectGeneration, spot)
                .whenComplete((ignored, failure) -> {
                    if (failure == null) {
                        candidate.complete(new Activation(selected, spot));
                    } else {
                        spot.close();
                        candidate.completeExceptionally(failure);
                        activations.remove(spotId, candidate);
                    }
                });
        } catch (Throwable failure) {
            candidate.completeExceptionally(failure);
            activations.remove(spotId, candidate);
        }
        return candidate;
    }

    boolean close(String spotId, long generation) {
        CompletableFuture<Activation> current = activations.get(spotId);
        if (current == null || !current.isDone()) {
            return false;
        }
        Activation activation;
        try {
            activation = current.join();
        } catch (RuntimeException failure) {
            return false;
        }
        if (activation.spot().lifecycleGeneration() != generation
            || !activations.remove(spotId, current)) {
            return false;
        }
        activation.spot().close();
        return true;
    }

    void closeAll() {
        activations.values().forEach(current -> {
            if (!current.isDone() || current.isCompletedExceptionally()) {
                return;
            }
            current.join().spot().close();
        });
        activations.clear();
        stableTypes.clear();
        factories.clear();
        hooks.clear();
    }

    private String selectType(
        String spotId,
        String requestedType) {
        String stored = stableTypes.get(spotId);
        if (stored != null) {
            if (requestedType != null && !stored.equals(requestedType)) {
                throw new IllegalStateException(
                    "Instance Spot stable type does not match authority");
            }
            return stored;
        }
        if (requestedType != null) {
            requireType(requestedType);
            if (!factories.containsKey(requestedType)) {
                throw new IllegalStateException(
                    "Instance Spot type is not registered: " + requestedType);
            }
            return requestedType;
        }
        if (factories.size() != 1) {
            throw new IllegalStateException(
                "Instance Spot type is required unless exactly one type is registered");
        }
        return factories.keySet().iterator().next();
    }

    private static void requireType(String value) {
        if (value == null
            || value.isBlank()
            || value.indexOf('\0') >= 0
            || value.getBytes(java.nio.charset.StandardCharsets.UTF_8).length
                > 0xff) {
            throw new IllegalArgumentException(
                "Instance Spot stable type must be text8");
        }
    }

    record Activation(String stableType, ZLinkBackendSpot spot) {
    }

    @FunctionalInterface
    interface ActivationHook {
        CompletionStage<Void> activate(
            String stableType,
            String spotId,
            long generation,
            ZLinkBackendSpot spot);
    }
}
