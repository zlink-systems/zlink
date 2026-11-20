package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.spots.*;

import java.lang.reflect.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

final class ZLinkPlacementDiscoveryFactoryTest {
    private static final AtomicInteger ACTORS = new AtomicInteger();
    private static final AtomicInteger SPOTS = new AtomicInteger();

    @RepeatedTest(40)
    void actualFactoriesRunOnceAfterDiscoveryAndReadyObjectsBypassPlacement() throws Exception {
        ACTORS.set(0);
        SPOTS.set(0);
        var store = new ZLinkInMemoryLocationStore();
        var state = new AtomicReference<ZLinkFrameworkRuntimeState>();
        var weight = new AtomicInteger(100);
        var lists = new AtomicInteger();
        var reserves = new AtomicInteger();
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("reserve"))
                                        reserves.incrementAndGet();
                                    if (method.getName().equals("listMeshNodes")) {
                                        lists.incrementAndGet();
                                        return store.listMeshNodes(
                                                        (String) args[0],
                                                        (ZLinkPageRequest) args[1])
                                                .thenApply(
                                                        page ->
                                                                new ZLinkLocationPage<>(
                                                                        page.items().stream()
                                                                                .map(
                                                                                        d ->
                                                                                                state
                                                                                                                        .get()
                                                                                                                == null
                                                                                                        ? d
                                                                                                        : new ZLinkMeshNodeDescriptor(
                                                                                                                d
                                                                                                                        .meshName(),
                                                                                                                d
                                                                                                                        .rid(),
                                                                                                                d
                                                                                                                        .lifecycleGeneration(),
                                                                                                                d
                                                                                                                        .descriptorRevision(),
                                                                                                                d
                                                                                                                        .endpoint(),
                                                                                                                d
                                                                                                                        .channelWeights(),
                                                                                                                d
                                                                                                                        .applicationVersion(),
                                                                                                                d
                                                                                                                        .objectCapabilities(),
                                                                                                                d
                                                                                                                        .objectRole(),
                                                                                                                d
                                                                                                                        .entrySpotId(),
                                                                                                                weight
                                                                                                                        .get(),
                                                                                                                d
                                                                                                                        .capacity(),
                                                                                                                d
                                                                                                                        .activationConcurrency(),
                                                                                                                d
                                                                                                                        .maintenanceWave(),
                                                                                                                state
                                                                                                                        .get(),
                                                                                                                d
                                                                                                                        .securityIdentity(),
                                                                                                                d
                                                                                                                        .ownerId(),
                                                                                                                d
                                                                                                                        .leaseGeneration(),
                                                                                                                d
                                                                                                                        .updatedAt()))
                                                                                .toList(),
                                                                        page.continuationToken()));
                                    }
                                    try {
                                        return method.invoke(store, args);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        var options = options(store);
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationStores(
                                        ZLinkRegisteredLocationStores.fromUnified(repository)))) {
            ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertTrue(runtime.isReady());
            state.set(ZLinkFrameworkRuntimeState.PREPARING);
            refresh(runtime);
            int before = lists.get();
            var actor =
                    runtime.actorManager()
                            .getOrCreate("actor", "player")
                            .timeout(Duration.ofSeconds(1))
                            .submit()
                            .toCompletableFuture();
            var spot =
                    runtime.spotManager()
                            .getOrCreate("spot", "room")
                            .timeout(Duration.ofSeconds(1))
                            .submit()
                            .toCompletableFuture();
            try {
                assertFalse(actor.isDone(), "Actor waits for discovery");
                assertFalse(spot.isDone(), "User Spot waits for discovery");
                assertEquals(before, lists.get(), "requests must not list the Store");
                assertEquals(0, reserves.get());
                assertEquals(0, ACTORS.get());
                assertEquals(0, SPOTS.get());
                state.set(ZLinkFrameworkRuntimeState.SERVING);
                refresh(runtime);
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class, actor.get(1, TimeUnit.SECONDS));
                assertEquals(ZLinkSpotCreateState.CREATED, spot.get(1, TimeUnit.SECONDS).state());
                assertEquals(1, ACTORS.get());
                assertEquals(1, SPOTS.get());
                assertEquals(2, reserves.get());
                state.set(ZLinkFrameworkRuntimeState.PREPARING);
                weight.set(0);
                refresh(runtime);
                assertInstanceOf(
                        ZLinkActorCreateResult.Existing.class,
                        runtime.actorManager()
                                .getOrCreate("actor", "player")
                                .submit()
                                .toCompletableFuture()
                                .get(1, TimeUnit.SECONDS));
                assertEquals(
                        ZLinkSpotCreateState.EXISTING,
                        runtime.spotManager()
                                .getOrCreate("spot", "room")
                                .submit()
                                .toCompletableFuture()
                                .get(1, TimeUnit.SECONDS)
                                .state());
                assertEquals(2, reserves.get());
                assertEquals(1, ACTORS.get());
                assertEquals(1, SPOTS.get());
            } finally {
                state.set(ZLinkFrameworkRuntimeState.SERVING);
                weight.set(100);
                refresh(runtime);
                CompletableFuture.allOf(
                                actor.handle((value, failure) -> null),
                                spot.handle((value, failure) -> null))
                        .join();
            }
        }
    }

    @RepeatedTest(40)
    void publicCreatesBeforeFirstPublicationCompleteAfterStartupDiscovery() throws Exception {
        ACTORS.set(0);
        SPOTS.set(0);
        var store = new ZLinkInMemoryLocationStore();
        var publication = new CompletableFuture<Void>();
        var firstStored = new CompletableFuture<Void>();
        var polling = new CompletableFuture<Void>();
        var lists = new AtomicInteger();
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("updateMeshNode"))
                                        return publication.thenCompose(
                                                ignored ->
                                                        store.updateMeshNode(
                                                                        (ZLinkMeshNodeDescriptor)
                                                                                args[0],
                                                                        (ZLinkDescriptorWriteIntent)
                                                                                args[1])
                                                                .thenApply(
                                                                        result -> {
                                                                            firstStored.complete(
                                                                                    null);
                                                                            return result;
                                                                        }));
                                    if (method.getName().equals("listMeshNodes")) {
                                        lists.incrementAndGet();
                                        return polling.thenCompose(
                                                ignored ->
                                                        store.listMeshNodes(
                                                                (String) args[0],
                                                                (ZLinkPageRequest) args[1]));
                                    }
                                    try {
                                        return method.invoke(store, args);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
        try (var runtime =
                ZLinkFrameworkRuntimeTestAccess.startWithConstruction(
                        options(store),
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        new ZLinkFrameworkRuntimeTestAccess.Construction()
                                .locationStores(
                                        ZLinkRegisteredLocationStores.fromUnified(repository)))) {
            try {
                int before = lists.get();
                var actor =
                        runtime.actorManager()
                                .create("first-actor", "player")
                                .timeout(Duration.ofSeconds(2))
                                .submit()
                                .toCompletableFuture();
                var spot =
                        runtime.spotManager()
                                .create("room")
                                .timeout(Duration.ofSeconds(2))
                                .submit()
                                .toCompletableFuture();
                assertFalse(actor.isDone());
                assertFalse(spot.isDone());
                assertEquals(before, lists.get(), "Create must not re-list the Store");
                publication.complete(null);
                firstStored.get(2, TimeUnit.SECONDS);
                polling.complete(null);
                ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime)
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
                refresh(runtime);
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class, actor.get(2, TimeUnit.SECONDS));
                assertEquals(ZLinkSpotCreateState.CREATED, spot.get(2, TimeUnit.SECONDS).state());
                assertEquals(1, ACTORS.get());
                assertEquals(1, SPOTS.get());
            } finally {
                publication.complete(null);
                polling.complete(null);
            }
        }
    }

    private static DefaultZLinkFrameworkOptions options(ZLinkInMemoryLocationStore store) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.configureLocations().setPollingInterval(Duration.ofSeconds(30));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh = options.addRouteMesh("mesh").listen("inproc://placement-" + UUID.randomUUID());
        mesh.objects()
                .server()
                .addEntrySpot(ZLinkNewActorEntryCandidateTest.PlayerEntry.class)
                .addActorFactory(
                        "player",
                        ZLinkNewActorEntryCandidateTest.Player.class,
                        Factory.class,
                        factory -> factory.disableRelocation())
                .addSpotFactory("room", Room.class, factory -> factory.disableRelocation());
        return options;
    }

    private static void refresh(ZLinkFrameworkRuntime runtime) {
        runtime.storeLocationResolvers
                .listPeers(ZLinkAutoConnectType.ROUTE_MESH, "mesh", null)
                .toCompletableFuture()
                .join();
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            ACTORS.incrementAndGet();
            return CompletableFuture.completedFuture(
                    new ZLinkNewActorEntryCandidateTest.Player(context));
        }
    }

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<ZLinkActor> {
        public Room {
            SPOTS.incrementAndGet();
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.reject());
        }

        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
