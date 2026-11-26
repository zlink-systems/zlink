package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotActorSendHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Predicate;

final class ZLinkActorDestroyContractTest {
    private static final String ACTOR_ID = "destroy-player";
    private static final String ACTOR_TYPE = "player";
    private static final String MESH = "destroy-mesh";
    private static CompletableFuture<Void> accepted;
    private static CompletableFuture<Void> release;

    @Test
    void getOrCreateWaitsForNextIncarnation() throws Exception {
        withClosingActor(
                (runtime, actor, locations) -> {
                    var next = runtime.actorManager().getOrCreate(ACTOR_ID, ACTOR_TYPE).submit();
                    assertThrows(
                            TimeoutException.class,
                            () -> next.toCompletableFuture().get(100, TimeUnit.MILLISECONDS));
                    release.complete(null);
                    assertTrue(
                            assertInstanceOf(ZLinkActorCreateResult.Created.class, await(next))
                                            .actor()
                                            .objectGeneration()
                                    > actor.objectGeneration());
                });
    }

    @Test
    void findOmitsClosingIncarnation() throws Exception {
        withClosingActor(
                (runtime, actor, locations) ->
                        assertTrue(await(runtime.actorManager().find(ACTOR_ID)).isEmpty()));
    }

    @Test
    void exclusiveCreateReportsClosingActorAtCapacity() throws Exception {
        withClosingActor(
                (runtime, actor, locations) -> {
                    var failure =
                            assertThrows(
                                    CompletionException.class,
                                    () ->
                                            runtime.actorManager()
                                                    .create(ACTOR_ID, ACTOR_TYPE)
                                                    .submit()
                                                    .toCompletableFuture()
                                                    .join());
                    assertEquals(
                            ZLinkFrameworkErrorKind.ALREADY_EXISTS,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind());
                });
    }

    @Test
    void followingDestroyJoinsTerminalAfterOwnTurnReturns() throws Exception {
        withClosingActor(
                (runtime, actor, locations) -> {
                    var following = runtime.actorManager().destroy(actor).toCompletableFuture();
                    assertThrows(
                            TimeoutException.class,
                            () -> following.get(100, TimeUnit.MILLISECONDS));
                    release.complete(null);
                    assertTrue(await(following));
                });
    }

    @Test
    void ownTurnReturnsAfterPublishingClosing() throws Exception {
        withClosingActor(
                (runtime, actor, locations) -> {
                    var snapshot =
                            assertInstanceOf(
                                    ZLinkAuthoritySnapshot.class,
                                    await(
                                            new ZLinkProviderLocationRepository(locations)
                                                    .read(
                                                            ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
                                                            () -> false)));
                    var payload =
                            new ZLinkActorAuthorityPayloadCodec()
                                    .decode(snapshot.payload())
                                    .orElseThrow();
                    assertEquals("CLOSING", payload.state().name());
                    assertEquals(actor.objectGeneration(), snapshot.objectGeneration());
                    assertEquals(actor.nodeRid(), payload.nodeRid());
                    assertFalse(release.isDone());
                });
    }

    @Test
    void remoteDestroyObservesOwnerAndRejectsPreviousGeneration() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        try (var owner = start(locations)) {
            var previous = create(owner);
            var endpoint = owner.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var remote = start(locations, "destroy-caller", endpoint)) {
                assertTimeoutPreemptively(
                        Duration.ofSeconds(10),
                        () -> {
                            while (owner.routeMeshRuntime().snapshot(MESH).readyPeerCount() == 0
                                    || remote.routeMeshRuntime().snapshot(MESH).readyPeerCount()
                                            == 0) {
                                Thread.sleep(10);
                            }
                        });
                var manager = remote.actorManager();
                assertTrue(await(manager.destroy(previous)));
                var current = create(owner);
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () -> manager.destroy(previous).toCompletableFuture().join());
                assertEquals(
                        ZLinkFrameworkErrorKind.INVALID_OPERATION,
                        assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
                assertEquals(current, await(owner.actorManager().find(ACTOR_ID)).orElseThrow());
            }
        }
    }

    @Test
    void closingCasFailureRestoresAdmissionAndAllowsNewDestroy() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var delegate = new ZLinkProviderLocationRepository(locations);
        String key = ZLinkAuthorityKeyCodec.actor(ACTOR_ID);
        var rejectClosing = new AtomicBoolean(true);
        var closingFailure = new IllegalStateException("injected Actor Closing CAS failure");
        var codec = new ZLinkActorAuthorityPayloadCodec();
        var repository =
                rejectActorMutation(
                        delegate,
                        mutation ->
                                rejectClosing.get()
                                        && mutation instanceof ZLinkAuthorityPut put
                                        && codec.decode(put.payload())
                                                .filter(
                                                        payload ->
                                                                payload.state()
                                                                        == ZLinkActorAuthorityPayloadCodec
                                                                                .State.CLOSING)
                                                .isPresent(),
                        closingFailure);
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .locationStores(ZLinkRegisteredLocationStores.fromUnified(repository));
        try (var runtime = start(locations, "destroy-owner", null, construction)) {
            var actor = create(runtime);
            var manager = runtime.actorManager();
            var first = manager.destroy(actor).toCompletableFuture();
            var failure = assertThrows(CompletionException.class, first::join);
            assertSame(closingFailure, failure.getCause());
            var snapshot =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class, await(delegate.read(key, () -> false)));
            assertTrue(snapshot.steadyReady());
            assertEquals(actor.objectGeneration(), snapshot.objectGeneration());
            assertEquals(
                    actor.objectGeneration(),
                    await(
                            runtime.actorClient()
                                    .requestToActor(ACTOR_ID, new ReadGeneration())
                                    .submit(Long.class)));

            rejectClosing.set(false);
            var following = manager.destroy(actor).toCompletableFuture();
            assertNotSame(first, following);
            assertTrue(await(following));
        }
    }

    @Test
    void destroyFailureRemainsClosingAndIsSharedWithLaterOwnerCalls() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var delegate = new ZLinkProviderLocationRepository(locations);
        String key = ZLinkAuthorityKeyCodec.actor(ACTOR_ID);
        var deleteFailure = new IllegalStateException("injected Actor destroy Delete failure");
        var repository =
                rejectActorMutation(
                        delegate,
                        mutation -> mutation instanceof ZLinkAuthorityDelete,
                        deleteFailure);
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .locationStores(ZLinkRegisteredLocationStores.fromUnified(repository));
        var runtime = start(locations, "destroy-owner", null, construction);
        boolean destroyFailureObserved = false;
        try {
            var actor = create(runtime);
            var manager = runtime.actorManager();
            var first =
                    assertThrows(
                            CompletionException.class,
                            () -> manager.destroy(actor).toCompletableFuture().join());
            var following =
                    assertThrows(
                            CompletionException.class,
                            () -> manager.destroy(actor).toCompletableFuture().join());
            assertSame(deleteFailure, first.getCause());
            assertSame(first.getCause(), following.getCause());
            destroyFailureObserved = true;
            assertTrue(await(manager.find(ACTOR_ID)).isEmpty());
            var snapshot =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class, await(delegate.read(key, () -> false)));
            assertEquals(
                    "CLOSING",
                    new ZLinkActorAuthorityPayloadCodec()
                            .decode(snapshot.payload())
                            .orElseThrow()
                            .state()
                            .name());
        } finally {
            if (destroyFailureObserved) {
                var shutdown = assertThrows(RuntimeException.class, runtime::close);
                assertSame(deleteFailure, shutdown.getCause());
            } else {
                runtime.close();
            }
        }
    }

    @Test
    void previousRefCannotDestroyReplacement() throws Exception {
        try (var runtime = start(new ZLinkInMemoryLocationStore())) {
            var manager = runtime.actorManager();
            var previous = create(runtime);
            assertTrue(await(manager.destroy(previous)));
            var current = create(runtime);
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () -> manager.destroy(previous).toCompletableFuture().join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            assertEquals(current, await(manager.find(ACTOR_ID)).orElseThrow());
        }
    }

    private static ZLinkLocationRepository rejectActorMutation(
            ZLinkLocationRepository delegate,
            Predicate<ZLinkAuthorityMutation> reject,
            RuntimeException failure) {
        String key = ZLinkAuthorityKeyCodec.actor(ACTOR_ID);
        return (ZLinkLocationRepository)
                Proxy.newProxyInstance(
                        ZLinkLocationRepository.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationRepository.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("compareExchange")
                                    && key.equals(arguments[0])
                                    && reject.test((ZLinkAuthorityMutation) arguments[2])) {
                                return CompletableFuture.failedFuture(failure);
                            }
                            try {
                                return method.invoke(delegate, arguments);
                            } catch (InvocationTargetException invocationFailure) {
                                throw invocationFailure.getCause();
                            }
                        });
    }

    private static void withClosingActor(ClosingCheck check) throws Exception {
        accepted = new CompletableFuture<>();
        release = new CompletableFuture<>();
        var locations = new ZLinkInMemoryLocationStore();
        try (var runtime = start(locations)) {
            var actor = create(runtime);
            try {
                await(runtime.actorClient().sendToActor(ACTOR_ID, new Destroy()).submit());
                await(accepted);
                check.run(runtime, actor, locations);
            } finally {
                release.complete(null);
            }
        }
    }

    private static ActorRef create(ZLinkFrameworkRuntime runtime) throws Exception {
        return assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        await(runtime.actorManager().create(ACTOR_ID, ACTOR_TYPE).submit()))
                .actor();
    }

    private static ZLinkFrameworkRuntime start(ZLinkInMemoryLocationStore locations)
            throws Exception {
        return start(locations, "destroy-owner", null);
    }

    private static ZLinkFrameworkRuntime start(
            ZLinkInMemoryLocationStore locations, String rid, String peerEndpoint)
            throws Exception {
        return start(
                locations, rid, peerEndpoint, new ZLinkFrameworkRuntimeTestAccess.Construction());
    }

    private static ZLinkFrameworkRuntime start(
            ZLinkInMemoryLocationStore locations,
            String rid,
            String peerEndpoint,
            ZLinkFrameworkRuntimeTestAccess.Construction construction)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.addLocationStore(locations);
        options.addRelocationStore(new InMemoryRelocationStore());
        var node = options.addRouteMesh(MESH).setActorCapacity(1);
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(rid));
        if (peerEndpoint == null) {
            var objects = node.objects().server();
            objects.addEntrySpot(Lobby.class);
            objects.addActorFactory(
                    ACTOR_TYPE,
                    Player.class,
                    Factory.class,
                    factory -> factory.recreateOnRelocation());
        } else {
            node.objects().client();
            node.peerConnections().connect(peerEndpoint);
        }
        var runtime =
                ZLinkFrameworkRuntimeTestAccess.startHostWithConstruction(
                        options,
                        new ZLinkJavaBackendAdapterFactory(),
                        ZLinkHandlerActivator.reflection(),
                        null,
                        construction);
        try {
            await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
            return runtime;
        } catch (Exception | Error failure) {
            runtime.close();
            throw failure;
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(10, TimeUnit.SECONDS);
    }

    private interface ClosingCheck {
        void run(
                ZLinkFrameworkRuntime runtime, ActorRef actor, ZLinkInMemoryLocationStore locations)
                throws Exception;
    }

    public record Destroy() {}

    public record ReadGeneration() {}

    public record Player(ZLinkActorContext context) implements ZLinkActor {}

    public static final class Factory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class Lobby implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public Lobby(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(DestroyHandler.class);
            context.handlers().addHandler(ReadGenerationHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ReadGenerationHandler
            implements ZLinkEntrySpotActorRequestHandler<Lobby, Player, ReadGeneration, Long> {
        @Override
        public CompletionStage<Long> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, ReadGeneration message) {
            return CompletableFuture.completedFuture(actor.context().objectGeneration());
        }
    }

    public static final class DestroyHandler
            implements ZLinkEntrySpotActorSendHandler<Lobby, Player, Destroy> {
        @Override
        public CompletionStage<Void> handle(
                Lobby spot, Player actor, ZLinkMessageContext context, Destroy message) {
            return spot.context()
                    .destroyActor(actor)
                    .whenComplete(
                            (ignored, error) -> {
                                if (error == null) accepted.complete(null);
                                else accepted.completeExceptionally(error);
                            })
                    .thenCompose(ignored -> release);
        }
    }
}
