package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTestAccess;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.spots.ZLinkSpot;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;

/** Test-only access to package-owned spot runtime collaborators. */
public final class ZLinkSpotRuntimeTestAccess {
    private ZLinkSpotRuntimeTestAccess() {}

    public static ThreadPoolExecutor multicastExecutor(ZLinkSpotRuntime runtime) {
        return runtime.publishers.multicastExecutor;
    }

    public static boolean actorQueuesEmpty(ZLinkSpotSerialExecutor executor) {
        return executor.actorQueues.isEmpty();
    }

    public static ZLinkDispatchErrorReporter dispatchErrors(ZLinkSpotRuntime runtime) {
        return runtime.dispatchErrors;
    }

    public static boolean hasActorTimerOwner(ZLinkSpot<?> spot, String actorId) {
        return ((DefaultSpotContext) spot.context()).actorTimers.containsKey(actorId);
    }

    public static int relocationStateMachineCount(Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines.size();
    }

    public static List<ZLinkCanonicalRelocationStateMachine> relocationStateMachines(
            Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines;
    }

    public static void replaceRelocationStateMachines(
            Object retireRuntime, List<ZLinkCanonicalRelocationStateMachine> stateMachines) {
        ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines = stateMachines;
    }

    public static Object firstRelocationTargetEndpoint(Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines.getFirst().target;
    }

    public static Object firstRelocationStateMachine(Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines.getFirst();
    }

    public static int targetAttemptCount(Object stateMachine) {
        return ((ZLinkCanonicalRelocationStateMachine) stateMachine).targets.size();
    }

    public static java.util.concurrent.CompletableFuture<Void> firstTargetAttemptTerminal(
            Object stateMachine) {
        return ((ZLinkCanonicalRelocationStateMachine) stateMachine)
                .targets
                .values()
                .iterator()
                .next()
                .terminal();
    }

    public static Object targetNormalizer(Object endpoint) {
        return ((ZLinkUserSpotRetireTargetEndpoint) endpoint).normalizer;
    }

    public static Class<?> targetNormalizerType() {
        return ZLinkUserSpotRetireTargetEndpoint.SteadyNormalizer.class;
    }

    public static void replaceTargetNormalizer(Object endpoint, Object normalizer) {
        ((ZLinkUserSpotRetireTargetEndpoint) endpoint).normalizer =
                (ZLinkUserSpotRetireTargetEndpoint.SteadyNormalizer) normalizer;
    }

    public static Object gateTargetNormalizer(
            Object endpoint,
            java.util.concurrent.CompletableFuture<Void> entered,
            java.util.concurrent.CompletableFuture<Void> release) {
        var retireEndpoint = (ZLinkUserSpotRetireTargetEndpoint) endpoint;
        var original = retireEndpoint.normalizer;
        retireEndpoint.normalizer =
                request ->
                        original.normalize(request)
                                .thenCompose(
                                        ignored -> {
                                            entered.complete(null);
                                            return release;
                                        });
        return original;
    }

    public static Object aggregateCoordinator(Object ingressHandler) {
        return ((ZLinkUserSpotRetireTargetEndpoint) ingressHandler).coordinator;
    }

    public static ThreadPoolExecutor replaceMulticastExecutor(
            ZLinkSpotRuntime runtime, ThreadPoolExecutor executor) {
        var previous = runtime.publishers.multicastExecutor;
        runtime.publishers.multicastExecutor = executor;
        return previous;
    }

    public static CompletionStage<?> readInstanceSpotActivationTarget(
            ZLinkSpotRuntime runtime,
            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope) {
        return runtime.readInstanceSpotActivationTarget(envelope);
    }

    public static CompletionStage<?> reserveInstanceSpotTarget(
            ZLinkSpotRuntime runtime,
            ZLinkInstanceActivationRecoveryCodec.RecoveryEnvelope envelope) {
        return runtime.reserveInstanceSpotTarget(envelope);
    }

    public static CompletionStage<?> resolveInstanceType(
            ZLinkSpotRuntime runtime,
            ZLinkLocationRepository store,
            String meshName,
            String requestedType) {
        return runtime.resolveInstanceType(store, meshName, requestedType);
    }

    public static CompletionStage<?> selectInstanceSpotTarget(
            ZLinkSpotRuntime runtime,
            ZLinkLocationRepository store,
            String meshName,
            String stableType) {
        return runtime.selectInstanceSpotTarget(store, meshName, stableType);
    }

    public static SpotTransportAddressResolver replaceSpotAddressResolver(
            ZLinkSpotRuntime runtime, SpotTransportAddressResolver resolver) {
        return ZLinkChannelRuntimeTestAccess.replaceSpotAddressResolver(runtime.channels, resolver);
    }

    public static Map<String, ZLinkInternalMeshNode> routeMeshNodesByName(
            ZLinkSpotRuntime runtime) {
        return runtime.routeMeshNodesByName;
    }

    public static ZLinkLocationRepository userSpotLocationStore(ZLinkSpotRuntime runtime) {
        return runtime.userSpotLocationStore;
    }

    public static Object userSpotOperationHandler(ZLinkSpotRuntime runtime, String meshName) {
        return runtime.userSpotOperationHandlers.get(meshName);
    }

    public static ZLinkLocationRepository authorityStore(Object operationHandler) {
        return ((ZLinkUserSpotOperationHandler) operationHandler).authorityStore;
    }

    public static void replaceAuthorityStore(
            Object operationHandler, ZLinkLocationRepository authorityStore) {
        ((ZLinkUserSpotOperationHandler) operationHandler).authorityStore = authorityStore;
    }

    public static ZLinkLocationRepository replaceUserSpotLocationStore(
            ZLinkSpotRuntime runtime, ZLinkLocationRepository store) {
        var previous = runtime.userSpotLocationStore;
        runtime.userSpotLocationStore = store;
        return previous;
    }

    public static ScheduledExecutorService replaceTimerExecutor(
            ZLinkSpotRuntime runtime, ScheduledExecutorService executor) {
        var previous = runtime.timerExecutor;
        runtime.timerExecutor = executor;
        return previous;
    }

    public static Executor replaceHandlerExecutor(ZLinkSpotRuntime runtime, Executor executor) {
        var previous = runtime.handlerExecutor;
        runtime.handlerExecutor = executor;
        return previous;
    }

    public static Executor replaceTimerLaneExecutor(Object context, Executor executor) {
        var lane =
                context instanceof DefaultSpotContext spotContext
                        ? spotContext.timers.stateLane
                        : ((DefaultInstanceSpotContext) context).timers.stateLane;
        return systems.zlink.framework.runtime.internal.execution.ZLinkStateLaneTestAccess
                .replaceExecutor(lane, executor);
    }
}
