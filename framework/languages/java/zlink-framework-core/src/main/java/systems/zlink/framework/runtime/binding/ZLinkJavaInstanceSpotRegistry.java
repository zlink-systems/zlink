package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.function.BiFunction;
import java.util.function.Consumer;
import java.util.function.Supplier;

/**
 * Framework-owned Instance Spot activation barrier. Only an Instance-marked direct operation calls
 * this registry; ordinary missing Spot routes do not.
 */
final class ZLinkJavaInstanceSpotRegistry {
    private final ZLinkStateLane ingressLane;
    private final Executor applicationExecutor;
    private final Map<String, BiFunction<String, Long, ZLinkBackendSpot>> factories =
            new ConcurrentHashMap<>();
    private final Map<String, ActivationHook> hooks = new ConcurrentHashMap<>();
    private final Map<String, String> stableTypes = new ConcurrentHashMap<>();
    private final Map<String, CompletableFuture<Activation>> activations =
            new ConcurrentHashMap<>();

    ZLinkJavaInstanceSpotRegistry(ZLinkStateLane ingressLane, Executor applicationExecutor) {
        this.ingressLane = Objects.requireNonNull(ingressLane, "ingressLane");
        this.applicationExecutor =
                Objects.requireNonNull(applicationExecutor, "applicationExecutor");
    }

    void register(String stableType, BiFunction<String, Long, ZLinkBackendSpot> factory) {
        register(
                stableType,
                factory,
                (type, spotId, generation, spot) -> CompletableFuture.completedFuture(null));
    }

    void register(
            String stableType,
            BiFunction<String, Long, ZLinkBackendSpot> factory,
            ActivationHook hook) {
        requireType(stableType);
        if (factories.putIfAbsent(stableType, Objects.requireNonNull(factory, "factory")) != null) {
            throw new IllegalStateException(
                    "Instance Spot type is already registered: " + stableType);
        }
        hooks.put(stableType, Objects.requireNonNull(hook, "hook"));
    }

    CompletionStage<Activation> activate(
            String spotId, String requestedType, long objectGeneration) {
        return activate(spotId, requestedType, objectGeneration, Long.MAX_VALUE);
    }

    CompletionStage<Activation> activate(
            String spotId, String requestedType, long objectGeneration, long deadlineUnixMs) {
        return activate(spotId, requestedType, objectGeneration, deadlineUnixMs, ignored -> {});
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            long objectGeneration,
            long deadlineUnixMs,
            Consumer<ZLinkBackendSpot> restoreFirst) {
        Objects.requireNonNull(spotId, "spotId");
        if (objectGeneration <= 0) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                            "Instance Spot object generation must be positive"));
        }
        return activate(
                spotId,
                requestedType,
                deadlineUnixMs,
                () -> CompletableFuture.completedFuture(objectGeneration),
                restoreFirst);
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            long deadlineUnixMs,
            Supplier<CompletionStage<Long>> generation,
            Consumer<ZLinkBackendSpot> restoreFirst) {
        Objects.requireNonNull(spotId, "spotId");
        String selected = selectType(spotId, requestedType);
        String recorded = stableTypes.putIfAbsent(spotId, selected);
        if (recorded != null && !recorded.equals(selected)) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Instance Spot stable type does not match authority"));
        }
        CompletableFuture<Activation> candidate = new CompletableFuture<>();
        return ingressLane
                .runAsync(() -> activations.put(spotId, candidate))
                .thenComposeAsync(
                        current ->
                                activateRegistered(
                                        spotId,
                                        selected,
                                        deadlineUnixMs,
                                        generation,
                                        restoreFirst,
                                        candidate,
                                        current),
                        applicationExecutor);
    }

    private CompletionStage<Activation> activateRegistered(
            String spotId,
            String selected,
            long deadlineUnixMs,
            Supplier<CompletionStage<Long>> generation,
            Consumer<ZLinkBackendSpot> restoreFirst,
            CompletableFuture<Activation> candidate,
            CompletableFuture<Activation> current) {
        if (current != null) {
            var result = new CompletableFuture<Activation>();
            current.whenCompleteAsync(
                    (value, failure) -> {
                        if (failure != null) {
                            result.completeExceptionally(failure);
                            candidate.completeExceptionally(failure);
                            activations.remove(spotId, candidate);
                            return;
                        }
                        try {
                            restoreFirst.accept(value.spot());
                            result.complete(value);
                        } catch (RuntimeException | Error refusal) {
                            result.completeExceptionally(refusal);
                        } finally {
                            // Preserve Ready even when this operation's queue admission fails.
                            candidate.complete(value);
                        }
                    },
                    applicationExecutor);
            return result;
        }
        try {
            Supplier<CompletionStage<Activation>> start =
                    () ->
                            generation
                                    .get()
                                    .thenComposeAsync(
                                            objectGeneration -> {
                                                ZLinkBackendSpot spot =
                                                        factories
                                                                .get(selected)
                                                                .apply(spotId, objectGeneration);
                                                if (spot == null) {
                                                    throw new IllegalStateException(
                                                            "Instance Spot factory returned null");
                                                }
                                                if (spot.lifecycleGeneration()
                                                        != objectGeneration) {
                                                    throw new IllegalStateException(
                                                            "Instance Spot factory returned a stale"
                                                                    + " generation");
                                                }
                                                return hooks.get(selected)
                                                        .activate(
                                                                selected,
                                                                spotId,
                                                                objectGeneration,
                                                                spot,
                                                                deadlineUnixMs,
                                                                restoreFirst)
                                                        .whenComplete(
                                                                (ignored, failure) -> {
                                                                    if (failure != null)
                                                                        spot.close();
                                                                })
                                                        .thenApply(
                                                                ignored ->
                                                                        new Activation(
                                                                                selected, spot));
                                            },
                                            applicationExecutor);
            hooks.get(selected)
                    .admit(start)
                    .whenComplete(
                            (value, failure) -> {
                                if (failure == null) {
                                    candidate.complete(value);
                                } else {
                                    candidate.completeExceptionally(failure);
                                    activations.remove(spotId, candidate);
                                }
                            });
        } catch (RuntimeException | Error failure) {
            candidate.completeExceptionally(failure);
            activations.remove(spotId, candidate);
        }
        return candidate;
    }

    CompletionStage<Void> admitExisting(
            String stableType,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage message,
            ZLinkBackendReceived received) {
        ActivationHook hook = hooks.get(stableType);
        return hook == null ? null : hook.admitExisting(message, received);
    }

    CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
            ZLinkAuthorityReadResult authority) {
        ActivationHook hook = hooks.get(envelope.stableType());
        if (hook == null)
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Instance type is not registered"));
        return hook.reserve(envelope, authority);
    }

    CompletionStage<Void> completed(
            String stableType, ZLinkServiceM6BWireCodec.InstanceSpotMessage message) {
        return hooks.get(stableType).completed(message);
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
        activations
                .values()
                .forEach(
                        current -> {
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

    private String selectType(String spotId, String requestedType) {
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
                || value.getBytes(StandardCharsets.UTF_8).length > 0xff) {
            throw new IllegalArgumentException("Instance Spot stable type must be text8");
        }
    }

    record Activation(String stableType, ZLinkBackendSpot spot) {}

    @FunctionalInterface
    interface ActivationHook {
        default <T> CompletionStage<T> admit(Supplier<CompletionStage<T>> work) {
            return work.get();
        }

        CompletionStage<Void> activate(
                String stableType, String spotId, long generation, ZLinkBackendSpot spot);

        default CompletionStage<Void> activate(
                String stableType,
                String spotId,
                long generation,
                ZLinkBackendSpot spot,
                long deadlineUnixMs,
                Consumer<ZLinkBackendSpot> restoreFirst) {
            return activate(stableType, spotId, generation, spot, deadlineUnixMs)
                    .thenRun(() -> restoreFirst.accept(spot));
        }

        default CompletionStage<Void> completed(
                ZLinkServiceM6BWireCodec.InstanceSpotMessage message) {
            return CompletableFuture.completedFuture(null);
        }

        default CompletionStage<Void> activate(
                String stableType,
                String spotId,
                long generation,
                ZLinkBackendSpot spot,
                long deadlineUnixMs) {
            return activate(stableType, spotId, generation, spot);
        }

        default CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
                ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
                ZLinkAuthorityReadResult authority) {
            return CompletableFuture.failedFuture(
                    new UnsupportedOperationException("Cold activation is unavailable"));
        }

        default CompletionStage<Void> admitExisting(
                ZLinkServiceM6BWireCodec.InstanceSpotMessage message,
                ZLinkBackendReceived received) {
            return null;
        }
    }
}
