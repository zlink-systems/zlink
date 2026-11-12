package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkInstanceActivationRecoveryCodec;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.spots.ZLinkSpot;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;

/** Test-only access to package-owned spot runtime collaborators. */
public final class ZLinkSpotRuntimeTestAccess {
    private ZLinkSpotRuntimeTestAccess() {}

    public static ThreadPoolExecutor multicastExecutor(ZLinkSpotRuntime runtime) {
        return runtime.publishers.multicastExecutor;
    }

    public static ZLinkSpotRuntime withTimerCollaborators(
            ZLinkBackendAdapterProvider backendFactory,
            ZLinkBackendAdapterOptions adapterOptions,
            ZLinkFrameworkRegistration registration,
            ZLinkChannelRuntime channels,
            ZLinkBackendContext context,
            ZLinkMessageSerializer serializer,
            ZLinkHandlerActivator handlerFactory,
            ZLinkRuntimeEventDispatcher eventDispatcher,
            Map<String, ZLinkInternalMeshNode> meshNodes,
            ScheduledExecutorService timerExecutor,
            ThreadPoolExecutor multicastExecutor,
            Executor handlerExecutor,
            Supplier<ZLinkStateLane> timerStateLaneFactory) {
        return new ZLinkSpotRuntime(
                backendFactory,
                adapterOptions,
                registration,
                channels,
                context,
                serializer,
                handlerFactory,
                eventDispatcher,
                meshNodes,
                timerExecutor,
                multicastExecutor,
                handlerExecutor,
                timerStateLaneFactory);
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

    public static Object firstRelocationTargetEndpoint(Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines.getFirst().target;
    }

    public static Object firstRelocationStateMachine(Object retireRuntime) {
        return ((ZLinkUserSpotRetireRuntime) retireRuntime).stateMachines.getFirst();
    }

    public static ZLinkUserSpotRetireRuntime userSpotRetireRuntimeWithStateMachines(
            ZLinkSpotRuntime spots,
            ZLinkActorRuntime actors,
            List<MeshNodeRegistration> registrations,
            Map<String, ZLinkInternalMeshNode> nodes,
            ZLinkLocationRepository locations,
            ZLinkStoreLocationResolvers locationResolvers,
            ZLinkLocationRepository authorities,
            ZLinkLocationOptions options,
            ZLinkRelocationAdapterRegistry adapters,
            long applicationVersion,
            Optional<String> maintenanceWave,
            Duration sessionRelocationSealTimeout,
            Predicate<String> targetAdmissionSealed,
            List<?> stateMachines,
            UnaryOperator<ZLinkLocationRepository> authorityStoreDecorator,
            UnaryOperator<Function<Object, CompletionStage<Void>>> targetNormalizerDecorator) {
        return new ZLinkUserSpotRetireRuntime(
                spots,
                actors,
                registrations,
                nodes,
                locations,
                locationResolvers,
                authorities,
                options,
                adapters,
                applicationVersion,
                maintenanceWave,
                sessionRelocationSealTimeout,
                targetAdmissionSealed,
                stateMachines,
                authorityStoreDecorator,
                targetNormalizerDecorator);
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

    public static Object aggregateCoordinator(Object ingressHandler) {
        return ((ZLinkUserSpotRetireTargetEndpoint) ingressHandler).coordinator;
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
            ZLinkSpotRuntime runtime, String meshName, String requestedType) {
        return runtime.resolveInstanceType(meshName, requestedType);
    }

    public static CompletionStage<?> selectInstanceSpotTarget(
            ZLinkSpotRuntime runtime, String meshName, String stableType) {
        return runtime.selectInstanceSpotTarget(meshName, stableType);
    }

    public static Map<String, ZLinkInternalMeshNode> routeMeshNodesByName(
            ZLinkSpotRuntime runtime) {
        return runtime.routeMeshNodesByName;
    }

    public static Object userSpotOperationHandler(ZLinkSpotRuntime runtime, String meshName) {
        return runtime.userSpotOperationHandlers.get(meshName);
    }

    public static ZLinkLocationRepository authorityStore(Object operationHandler) {
        return ((ZLinkUserSpotOperationHandler) operationHandler).authorityStore;
    }
}
