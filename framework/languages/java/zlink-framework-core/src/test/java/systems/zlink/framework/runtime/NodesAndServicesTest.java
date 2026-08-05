package systems.zlink.framework.runtime.host;

import systems.zlink.framework.spots.SpotHandleResolver;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.configuration.ZLinkUserSpotFactoryBuilder;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpotKind;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

final class NodesAndServicesTest {
    @Test
    void factoryBuilderRequiresOneRelocationChoiceAndKeepsDocumentedDefaults() {
        var missing = new DefaultZLinkFrameworkOptions();
        var missingObjects = missing.addRouteMesh("missing-policy")
            .objects()
            .server();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> missingObjects.addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> factory.stableTypeLimit(12)));

        var duplicate = new DefaultZLinkFrameworkOptions();
        var duplicateObjects = duplicate.addRouteMesh("duplicate-policy")
            .objects()
            .server();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> duplicateObjects.addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> {
                    factory.disableRelocation();
                    factory.recreateOnRelocation();
                }));

        var perActor = new DefaultZLinkFrameworkOptions();
        var perActorObjects = perActor.addRouteMesh("per-actor-readiness")
            .objects()
            .server();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> perActorObjects.addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> {
                    factory.executionMode(ZLinkUserSpotExecutionMode.PER_ACTOR);
                    factory.relocationReadiness(
                        ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED);
                    factory.disableRelocation();
                }));

        var perActorDisabled = new DefaultZLinkFrameworkOptions();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> perActorDisabled.addRouteMesh("per-actor-disabled")
                .objects()
                .server()
                .addSpotFactory(
                    "room",
                    RoomSpot.class,
                    factory -> {
                        factory.executionMode(ZLinkUserSpotExecutionMode.PER_ACTOR);
                        factory.disableRelocation();
                    }));

        var perActorPreserved = new DefaultZLinkFrameworkOptions();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> perActorPreserved.addRouteMesh("per-actor-preserved")
                .objects()
                .server()
                .addSpotFactory(
                    "room",
                    RoomSpot.class,
                    factory -> {
                        factory.executionMode(ZLinkUserSpotExecutionMode.PER_ACTOR);
                        factory.preserveStateWith(RoomRelocationAdapter.class);
                    }));

        var nullAdapter = new DefaultZLinkFrameworkOptions();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> nullAdapter.addRouteMesh("null-adapter")
                .objects()
                .server()
                .addSpotFactory(
                    "room",
                    RoomSpot.class,
                    factory -> factory.preserveStateWith(null)));

        var invalidLimit = new DefaultZLinkFrameworkOptions();
        var invalidLimitObjects = invalidLimit.addRouteMesh("invalid-limit")
            .objects()
            .server();
        assertThrows(
            ZLinkConfigurationException.class,
            () -> invalidLimitObjects.addSpotFactory(
                "room-zero",
                RoomSpot.class,
                factory -> factory
                    .stableTypeLimit(0)
                    .disableRelocation()));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> invalidLimitObjects.addSpotFactory(
                "room-negative",
                RoomSpot.class,
                factory -> factory
                    .stableTypeLimit(-1)
                    .disableRelocation()));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> invalidLimitObjects.addInstanceSpotFactory(
                "instance-zero",
                RoomInstanceSpot.class,
                factory -> factory
                    .stableTypeLimit(0)
                    .disableRelocation()));

        @SuppressWarnings("unchecked")
        ZLinkUserSpotFactoryBuilder<RoomSpot>[] escaped =
            (ZLinkUserSpotFactoryBuilder<RoomSpot>[]) new ZLinkUserSpotFactoryBuilder<?>[1];
        var escapedBuilder = new DefaultZLinkFrameworkOptions();
        escapedBuilder.addRouteMesh("escaped-builder")
            .objects()
            .server()
            .addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> {
                    escaped[0] = factory;
                    factory.disableRelocation();
                });
        assertThrows(
            ZLinkConfigurationException.class,
            () -> escaped[0].stableTypeLimit(5));

        var callbackFailure = new DefaultZLinkFrameworkOptions();
        var callbackFailureObjects = callbackFailure
            .addRouteMesh("callback-failure")
            .objects()
            .server();
        var originalFailure = new IllegalStateException("configure failed");
        assertEquals(
            originalFailure,
            assertThrows(
                IllegalStateException.class,
                () -> callbackFailureObjects.addSpotFactory(
                    "room",
                    RoomSpot.class,
                    factory -> {
                        factory.disableRelocation();
                        throw originalFailure;
                    })));
        assertTrue(callbackFailure.registration()
            .meshNodes()
            .getFirst()
            .relocatableSpotFactories()
            .isEmpty());

        var defaults = new DefaultZLinkFrameworkOptions();
        defaults.addRouteMesh("factory-defaults")
            .objects()
            .server()
            .addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> factory.disableRelocation());
        var configuration = defaults.registration()
            .meshNodes()
            .getFirst()
            .relocatableSpotFactories()
            .get("room")
            .options();
        assertEquals(0, configuration.stableTypeLimit());
        assertEquals(
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            configuration.executionMode());
        assertEquals(
            ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY,
            configuration.relocationReadiness());
    }

    @Test
    void addZLinkFramework_throws_whenSpotFactoryTypeIsDuplicatedOnMeshNode() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();

        assertThrows(ZLinkConfigurationException.class, () ->
            { var objects = options.addRouteMesh("game")
                    .listen("inproc://duplicate-spot")
                    .objects()
                    .server();
                objects.addSpotFactory(
                    "game",
                    GameSpot.class,
                    factory -> factory.disableRelocation());
                objects.addSpotFactory(
                    "game",
                    GameSpot.class,
                    factory -> factory.disableRelocation());
                options.validate(); });
    }

    @Test
    void addZLinkFramework_throws_whenMeshNodeRegistersMultipleEntrySpots() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();

        { var objects = options.addRouteMesh("game")
                .listen("inproc://multiple-entry-spots")
                .objects()
                .server();
            objects.addEntrySpot(EntrySpotA.class);
            objects.addEntrySpot(EntrySpotB.class); }

        assertThrows(ZLinkConfigurationException.class, options::validate);
    }

    @Test
    void addZLinkFramework_throws_whenMultipleMeshNodesOwnActorFactories() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();

        options.addRouteMesh("alpha")
            .listen("inproc://actor-alpha")
            .objects()
            .server()
            .addActorFactory(
                "player",
                PlayerActor.class,
                PlayerActorFactory.class,
                factory -> factory.disableRelocation());
        options.addRouteMesh("beta")
            .listen("inproc://actor-beta")
            .objects()
            .server()
            .addActorFactory(
                "mage",
                PlayerActor.class,
                PlayerActorFactory.class,
                factory -> factory.disableRelocation());

        assertThrows(ZLinkConfigurationException.class, options::validate);
    }

    @Test
    void addZLinkFramework_registersActorManager_whenSpotNodeAndActorFactoryExist() {
        DefaultZLinkFrameworkOptions options = optionsWithSpotNodeAndActorFactory();

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory())) {
            systems.zlink.framework.actors.ZLinkActorCreateResult result = runtime.actorManager()
                .create("player-1", "player")
                .submit()
                .toCompletableFuture()
                .join();
            ActorRef actor = ((systems.zlink.framework.actors.ZLinkActorCreateResult.Created) result)
                .actor();

            assertEquals("player-1", actor.actorId());
            assertTrue(
                runtime.actorManager()
                    .getOrCreate("player-1", "player")
                    .submit()
                    .toCompletableFuture()
                    .join()
                    instanceof systems.zlink.framework.actors.ZLinkActorCreateResult.Existing);
        }
    }

    @Test
    void concurrentGetOrCreateWaitsForCreatingThenReturnsExisting() {
        BlockingPlayerActorFactory.reset();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh("game")
            .listen("inproc://creating-actor")
            .setRoutingIdPrefix("creating-node");
        mesh.channelName("game").server();
        mesh.objects()
            .server()
            .addActorFactory(
                "blocking-player",
                PlayerActor.class,
                BlockingPlayerActorFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(
                     options,
                     new ZLinkJavaBackendAdapterFactory())) {
            CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult>
                first = runtime.actorManager().getOrCreate(
                    "player-serial",
                    "blocking-player").submit();
            CompletionStage<systems.zlink.framework.actors.ZLinkActorCreateResult>
                second = runtime.actorManager().getOrCreate(
                    "player-serial",
                    "blocking-player").submit();

            assertEquals(1, BlockingPlayerActorFactory.invocations.get());
            assertTrue(!second.toCompletableFuture().isDone());

            BlockingPlayerActorFactory.release.complete(null);

            assertTrue(first.toCompletableFuture().join()
                instanceof systems.zlink.framework.actors
                    .ZLinkActorCreateResult.Created);
            assertTrue(second.toCompletableFuture().join()
                instanceof systems.zlink.framework.actors
                    .ZLinkActorCreateResult.Existing);
            assertEquals(1, BlockingPlayerActorFactory.invocations.get());
        }
    }

    @Test
    void addZLinkFramework_allowsStandaloneLocalMeshNode() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());

        options.addRouteMesh("game")
            .listen("inproc://standalone-local-mesh")
            .objects()
            .server()
            .addSpotFactory(
                "game",
                GameSpot.class,
                factory -> factory.disableRelocation());

        assertDoesNotThrow(options::validate);
    }

    @Test
    void routeMeshDispatchesSpotRequestToTargetSpot() throws Exception {
        String suffix = Long.toUnsignedString(System.nanoTime());
        RoutingId nodeRid = RoutingId.from("game-node-" + suffix);
        RoutingId roomRid = RoutingId.from("room-" + suffix);
        ClientSpot.reply = new CompletableFuture<>();
        PingHandler.received = new CompletableFuture<>();
        FlowObserver.events.clear();
        ClientSpot.targetRoomRid = roomRid;
        ClientSpot.targetNodeRid = nodeRid;
        ClientSpot.targetMeshName = "game-" + suffix;

        DefaultZLinkFrameworkOptions options = routeMeshOptions();
        ZLinkInMemoryLocationStore sharedLocations = new ZLinkInMemoryLocationStore();
        options.addLocationStore(sharedLocations);
        options.addRelocationStore(new InMemoryRelocationStore());
        var mesh = options.addRouteMesh("game-" + suffix)
            .setRoutingIdPrefix(nodeRid.toString())
            .listen("inproc://route-mesh-request-" + suffix);
        mesh.channelName("game").server();
        mesh.objects().server()
            .addSpotFactory(
                "room",
                RoomSpot.class,
                factory -> factory.disableRelocation())
            .addSpotFactory(
                "client",
                ClientSpot.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                 ZLinkFrameworkRuntimeTestAccess.start(options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.spotManager()
                .getOrCreate(roomRid.toString(), "room")
                .submit()
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            runtime.spotManager()
                .create("client")
                .submit()
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);

            try {
                assertEquals("ping", PingHandler.received.get(2, TimeUnit.SECONDS));
            } catch (TimeoutException ex) {
                throw new AssertionError(
                    "message flow: " + FlowObserver.events
                        + ", source reply: " + futureState(ClientSpot.reply),
                    ex);
            }
            assertEquals(
                "pong:ping",
                ClientSpot.reply.get(2, TimeUnit.SECONDS).value());
        }
    }

    private static String futureState(CompletableFuture<?> future) {
        if (!future.isDone()) {
            return "pending";
        }
        try {
            return "completed:" + future.getNow(null);
        } catch (java.util.concurrent.CompletionException ex) {
            return "failed:" + ex.getCause();
        }
    }

    @Test
    void addZLinkFramework_throws_whenStreamNodeRegistersMultipleSessions() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();

        assertThrows(ZLinkConfigurationException.class, () ->
            { var stream = options.addStreamNode("gateway"); stream.bind("inproc://gateway");
                stream.registerSession(GameSession.class);
                stream.registerSession(GameSession.class); });
    }

    private static DefaultZLinkFrameworkOptions optionsWithSpotNodeAndActorFactory() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var mesh = options.addRouteMesh("game")
            .listen("inproc://play-router")
            .setRoutingIdPrefix("play-node");
        mesh.channelName("game").server();
        mesh.objects()
            .server()
            .addSpotFactory(
                "game",
                GameSpot.class,
                factory -> factory.disableRelocation())
            .addActorFactory(
                "player",
                PlayerActor.class,
                PlayerActorFactory.class,
                factory -> factory.disableRelocation());
        return options;
    }

    private static DefaultZLinkFrameworkOptions routeMeshOptions() {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(2));
        options.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
            .setMessageFlowObserver(new FlowObserver());
        return options;
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record Ping(String value) {
    }

    public record Pong(String value) {
    }

    public static final class FlowObserver implements ZLinkMessageFlowObserver {
        static final List<String> events = new CopyOnWriteArrayList<>();

        @Override
        public CompletionStage<Void> onMessageFlow(ZLinkMessageFlowEvent flow) {
            events.add(flow.outcome() + ":"
                + flow.surface() + ":"
                + flow.messageKind() + ":"
                + flow.packetName() + ":"
                + flow.channelName() + ":"
                + flow.errorReason());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RoomSpot implements ZLinkSpot<ZLinkActor> {
        private final ZLinkSpotContext context;

        public RoomSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(PingHandler.class);
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RoomInstanceSpot implements ZLinkInstanceSpot {
        @Override
        public ZLinkInstanceSpotContext context() {
            return null;
        }
    }

    public static final class RoomRelocationAdapter
        implements ZLinkSpotRelocationAdapter<RoomSpot> {
        @Override
        public CompletionStage<byte[]> capture(
            RoomSpot spot,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
            RoomSpot spot,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PingHandler
        implements ZLinkSpotRequestHandler<RoomSpot, Ping, Pong> {
        static CompletableFuture<String> received = new CompletableFuture<>();

        @Override
        public CompletionStage<Pong> handle(RoomSpot spot, Ping request) {
            received.complete(request.value());
            return CompletableFuture.completedFuture(new Pong("pong:" + request.value()));
        }
    }

    public static final class ClientSpot implements ZLinkSpot<ZLinkActor> {
        static CompletableFuture<Pong> reply = new CompletableFuture<>();
        static RoutingId targetRoomRid;
        static RoutingId targetNodeRid;
        static String targetMeshName;
        private final ZLinkSpotContext context;
        private final SpotHandleResolver handles;

        public ClientSpot(ZLinkSpotContext context, SpotHandleResolver handles) {
            this.context = context;
            this.handles = handles;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onInitialize() {
            return handles.resolveSpotHandle(targetMeshName, targetRoomRid.toString())
                .thenCompose(handle -> {
                    handle.orElseThrow(() ->
                        new IllegalStateException("target Spot handle not found"));
                    return context.outbound()
                        .requestToSpot(targetRoomRid.toString(), new Ping("ping"))
                        .timeout(Duration.ofSeconds(2))
                        .submit(Pong.class);
                })
                .whenComplete((value, error) -> {
                    if (error != null) {
                        reply.completeExceptionally(error);
                    } else {
                        reply.complete(value);
                    }
                })
                .thenApply(ignored -> null);
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EntrySpotA implements ZLinkEntrySpot<ZLinkActor> {
        @Override
        public ZLinkEntrySpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EntrySpotB implements ZLinkEntrySpot<ZLinkActor> {
        @Override
        public ZLinkEntrySpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;

        PlayerActor(String actorId, ZLinkActorContext context) {
            this.actorId = actorId;
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new PlayerActor(context.actorId(), context));
        }
    }

    public static final class BlockingPlayerActorFactory
        implements ZLinkActorFactory {
        static final AtomicInteger invocations = new AtomicInteger();
        static CompletableFuture<Void> release;

        static void reset() {
            invocations.set(0);
            release = new CompletableFuture<>();
        }

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            invocations.incrementAndGet();
            return release.thenApply(
                ignored -> new PlayerActor(context.actorId(), context));
        }
    }

    public static final class GameSession implements ZLinkSession {
        @Override
        public ZLinkSessionContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
