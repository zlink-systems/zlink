package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.InstanceSpotActivationHandler;
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
import java.util.function.Supplier;

/**
 * Framework-owned Instance Spot activation barrier. Only an Instance-marked direct operation calls
 * this registry; ordinary missing Spot routes do not.
 */
final class ZLinkJavaInstanceSpotRegistry {
    private final ZLinkStateLane ingressLane;
    private final Executor applicationExecutor;
    private final String meshName;

    private record Registration(
            BiFunction<String, Long, ZLinkBackendSpot> factory,
            InstanceSpotActivationHandler hook,
            systems.zlink.framework.runtime.internal.metrics.ZLinkInstanceSpotMetrics metrics) {}

    private final Map<String, Registration> factories = new ConcurrentHashMap<>();
    private final Map<String, String> stableTypes = new ConcurrentHashMap<>();
    private final Map<String, ActivationOperation> activations = new ConcurrentHashMap<>();

    ZLinkJavaInstanceSpotRegistry(ZLinkStateLane ingressLane, Executor applicationExecutor) {
        this(ingressLane, applicationExecutor, null);
    }

    ZLinkJavaInstanceSpotRegistry(
            ZLinkStateLane ingressLane, Executor applicationExecutor, String meshName) {
        this.meshName = meshName;
        this.ingressLane = Objects.requireNonNull(ingressLane, "ingressLane");
        this.applicationExecutor =
                Objects.requireNonNull(applicationExecutor, "applicationExecutor");
    }

    void register(String stableType, BiFunction<String, Long, ZLinkBackendSpot> factory) {
        register(
                stableType,
                factory,
                (type, route, spot) -> CompletableFuture.completedFuture(null));
    }

    void register(
            String stableType,
            BiFunction<String, Long, ZLinkBackendSpot> factory,
            InstanceSpotActivationHandler hook) {
        requireType(stableType);
        factories.compute(
                stableType,
                (type, previous) -> {
                    if (previous != null)
                        throw new IllegalStateException(
                                "Instance Spot type is already registered: " + stableType);
                    return new Registration(
                            Objects.requireNonNull(factory, "factory"),
                            Objects.requireNonNull(hook, "hook"),
                            meshName == null
                                    ? null
                                    : new systems.zlink.framework.runtime.internal.metrics
                                            .ZLinkInstanceSpotMetrics(meshName, stableType));
                });
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route) {
        return activate(
                spotId,
                requestedType,
                route,
                Long.MAX_VALUE,
                (ignored, admission) -> {
                    admission.complete(null);
                    return CompletableFuture.completedFuture(null);
                });
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            long deadlineUnixMs,
            BiFunction<ZLinkBackendSpot, CompletableFuture<Void>, CompletionStage<Void>>
                    restoreFirst) {
        Objects.requireNonNull(spotId, "spotId");
        Objects.requireNonNull(route, "route");
        return activate(
                spotId,
                requestedType,
                deadlineUnixMs,
                () -> CompletableFuture.completedFuture(route),
                restoreFirst);
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            long deadlineUnixMs,
            BiFunction<ZLinkBackendSpot, CompletableFuture<Void>, CompletionStage<Void>>
                    restoreFirst,
            long payloadBytes) {
        return activate(
                spotId,
                requestedType,
                deadlineUnixMs,
                () -> CompletableFuture.completedFuture(route),
                restoreFirst,
                payloadBytes);
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            long deadlineUnixMs,
            Supplier<CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence>> authority,
            BiFunction<ZLinkBackendSpot, CompletableFuture<Void>, CompletionStage<Void>>
                    restoreFirst) {
        return activate(spotId, requestedType, deadlineUnixMs, authority, restoreFirst, 0L);
    }

    CompletionStage<Activation> activate(
            String spotId,
            String requestedType,
            long deadlineUnixMs,
            Supplier<CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence>> authority,
            BiFunction<ZLinkBackendSpot, CompletableFuture<Void>, CompletionStage<Void>>
                    restoreFirst,
            long payloadBytes) {
        Objects.requireNonNull(spotId, "spotId");
        String selected = selectType(spotId, requestedType);
        String recorded = stableTypes.putIfAbsent(spotId, selected);
        if (recorded != null && !recorded.equals(selected)) {
            var metrics = factories.get(selected).metrics();
            if (metrics != null) metrics.conflict("spot_type");
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Instance Spot stable type does not match authority"));
        }
        ActivationOperation candidate = new ActivationOperation();
        return ingressLane
                .runAsync(() -> activations.put(spotId, candidate))
                .thenComposeAsync(
                        current -> {
                            CompletionStage<Activation> result =
                                    activateRegistered(
                                            spotId,
                                            selected,
                                            deadlineUnixMs,
                                            authority,
                                            restoreFirst,
                                            candidate,
                                            current,
                                            payloadBytes);
                            CompletableFuture.allOf(
                                            current == null
                                                    ? CompletableFuture.completedFuture(null)
                                                    : current.terminal,
                                            result.handle((value, failure) -> null)
                                                    .toCompletableFuture())
                                    .whenComplete(
                                            (value, failure) -> {
                                                if (candidate.isCompletedExceptionally())
                                                    activations.remove(spotId, candidate);
                                                candidate.terminal.complete(null);
                                            });
                            return result;
                        },
                        applicationExecutor);
    }

    private CompletionStage<Activation> activateRegistered(
            String spotId,
            String selected,
            long deadlineUnixMs,
            Supplier<CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence>> authority,
            BiFunction<ZLinkBackendSpot, CompletableFuture<Void>, CompletionStage<Void>>
                    restoreFirst,
            ActivationOperation candidate,
            ActivationOperation current,
            long payloadBytes) {
        var metrics = factories.get(selected).metrics();
        boolean cold = current == null || !current.isDone();
        long startedNanos = current == null && metrics != null ? System.nanoTime() : 0L;
        if (metrics != null && cold) {
            metrics.retain(payloadBytes);
            candidate.whenComplete(
                    (activation, failure) -> {
                        metrics.release(payloadBytes);
                        if (current == null) metrics.terminal(startedNanos, failure);
                    });
        }
        if (current != null) {
            var result = new CompletableFuture<Activation>();
            current.whenCompleteAsync(
                    (value, failure) -> {
                        if (failure != null) {
                            candidate.completeExceptionally(failure);
                            result.completeExceptionally(failure);
                            return;
                        }
                        // This operation's queue admission is submitted first, so joined
                        // operations enter in arrival order (06-spot-address-messaging.ko.md:423).
                        // The registry then records Ready, which also releases the next joined
                        // operation, before this caller observes the join
                        // (09-object-lifecycle.ko.md:134-137). Ready stays even when this
                        // operation's queue admission fails.
                        CompletionStage<Void> admission;
                        try {
                            admission =
                                    Objects.requireNonNull(
                                            restoreFirst.apply(
                                                    value.spot(), new CompletableFuture<>()),
                                            "Instance Spot admission returned null");
                        } catch (RuntimeException | Error refusal) {
                            admission = CompletableFuture.failedFuture(refusal);
                        }
                        candidate.complete(value);
                        admission.whenComplete(
                                (ignored, terminalFailure) -> {
                                    if (terminalFailure == null) result.complete(value);
                                    else result.completeExceptionally(terminalFailure);
                                });
                    },
                    applicationExecutor);
            return result;
        }
        var firstTerminal = new CompletableFuture<Void>();
        try {
            Supplier<CompletionStage<Activation>> start =
                    () ->
                            authority
                                    .get()
                                    .thenComposeAsync(
                                            route -> {
                                                ZLinkBackendSpot spot =
                                                        factories
                                                                .get(selected)
                                                                .factory()
                                                                .apply(
                                                                        spotId,
                                                                        route.objectGeneration());
                                                if (spot == null) {
                                                    throw new IllegalStateException(
                                                            "Instance Spot factory returned null");
                                                }
                                                if (spot.lifecycleGeneration()
                                                        != route.objectGeneration()) {
                                                    throw new IllegalStateException(
                                                            "Instance Spot factory returned a stale"
                                                                    + " generation");
                                                }
                                                return factories
                                                        .get(selected)
                                                        .hook()
                                                        .activate(
                                                                selected,
                                                                route,
                                                                spot,
                                                                deadlineUnixMs,
                                                                (activated, admission) ->
                                                                        restoreFirst
                                                                                .apply(
                                                                                        activated,
                                                                                        admission)
                                                                                .whenComplete(
                                                                                        (ignored,
                                                                                                failure) -> {
                                                                                            if (failure
                                                                                                    == null)
                                                                                                firstTerminal
                                                                                                        .complete(
                                                                                                                null);
                                                                                            else
                                                                                                firstTerminal
                                                                                                        .completeExceptionally(
                                                                                                                failure);
                                                                                        }))
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
            factories
                    .get(selected)
                    .hook()
                    .admit(start)
                    .whenComplete(
                            (value, failure) -> {
                                if (failure == null) {
                                    candidate.complete(value);
                                } else {
                                    candidate.completeExceptionally(failure);
                                }
                            });
        } catch (RuntimeException | Error failure) {
            candidate.completeExceptionally(failure);
        }
        return candidate.thenCompose(value -> firstTerminal.thenApply(ignored -> value));
    }

    CompletionStage<Void> admitExisting(
            String stableType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence currentRoute,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage message,
            ZLinkBackendReceived received) {
        Registration registered = factories.get(stableType);
        InstanceSpotActivationHandler hook = registered == null ? null : registered.hook();
        return hook == null ? null : hook.admitExisting(currentRoute, message, received);
    }

    CompletionStage<ZLinkServiceM6BWireCodec.InstanceRouteFence> reserve(
            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope,
            ZLinkAuthorityReadResult authority) {
        Registration registered = factories.get(envelope.stableType());
        InstanceSpotActivationHandler hook = registered == null ? null : registered.hook();
        if (hook == null)
            return CompletableFuture.failedFuture(
                    new IllegalStateException("Instance type is not registered"));
        return hook.reserve(envelope, authority);
    }

    CompletionStage<Void> recovered(
            String stableType,
            ZLinkServiceM6BWireCodec.InstanceRouteFence route,
            Throwable failure) {
        return factories.get(stableType).hook().recovered(route, failure);
    }

    CompletionStage<Void> completed(
            String stableType, ZLinkServiceM6BWireCodec.InstanceSpotMessage message) {
        return factories.get(stableType).hook().completed(message);
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

    CompletionStage<Void> terminals() {
        return CompletableFuture.allOf(
                activations.values().stream()
                        .map(operation -> operation.terminal)
                        .toArray(CompletableFuture[]::new));
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
        factories
                .values()
                .forEach(
                        registration -> {
                            if (registration.metrics() != null) registration.metrics().close();
                        });
        stableTypes.clear();
        factories.clear();
    }

    private String selectType(String spotId, String requestedType) {
        String stored = stableTypes.get(spotId);
        if (stored != null) {
            if (requestedType != null && !stored.equals(requestedType)) {
                Registration registration = factories.get(requestedType);
                if (registration != null && registration.metrics() != null)
                    registration.metrics().conflict("spot_type");
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

    private static final class ActivationOperation extends CompletableFuture<Activation> {
        final CompletableFuture<Void> terminal = new CompletableFuture<>();
    }

    record Activation(String stableType, ZLinkBackendSpot spot) {}
}
