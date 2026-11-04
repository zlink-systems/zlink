package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;

/** Transient collaborator injection for package-owned runtime construction and its tests. */
record ZLinkFrameworkRuntimeConstruction(
        Supplier<ZLinkMeshDrainCoordinator> meshDrains,
        ExecutorService channelInfrastructureExecutor,
        SpotTransportAddressResolver channelSpotAddressResolver,
        ZLinkLocationRuntime locationRuntime,
        ScheduledExecutorService spotTimerExecutor,
        ThreadPoolExecutor spotMulticastExecutor,
        Executor spotHandlerExecutor,
        Supplier<ZLinkStateLane> spotTimerStateLaneFactory,
        java.util.function.UnaryOperator<ZLinkLocationRepository> spotOperationAuthorityStore,
        ZLinkLocationRepository spotLocationStore,
        ZLinkLocationRuntime spotLocationRuntime,
        ZLinkStoreLocationResolvers spotLocationResolvers,
        List<?> relocationStateMachines,
        java.util.function.UnaryOperator<ZLinkLocationRepository>
                aggregateRelocationAuthorityStoreDecorator,
        UnaryOperator<Function<Object, CompletionStage<Void>>> targetNormalizerDecorator) {
    static ZLinkFrameworkRuntimeConstruction defaults() {
        return new ZLinkFrameworkRuntimeConstruction(
                null, null, null, null, null, null, null, null, null, null, null, null, null, null,
                null);
    }
}
