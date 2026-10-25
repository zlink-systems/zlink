package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkStoreSpotHandleResolver;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Spec 05-spot-actor-membership §4.1–4.2: the Entry Spot Join operation owns its request Message
 * until the target submission stage consumes it, also when the Entry Spot target resolves after the
 * call returns.
 */
final class ZLinkEntrySpotJoinPayloadOwnershipTest {
    private static final String ACTOR_ID = "entry-join-player";
    private static final AtomicReference<ZLinkActorContext> PLAYER_CONTEXT =
            new AtomicReference<>();

    @Test
    void entrySpotJoinKeepsRequestUntilAsynchronousTargetResolutionSubmitsIt() throws Exception {
        PLAYER_CONTEXT.set(null);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        var node = options.addRouteMesh("entry-join");
        node.listen("inproc://entry-join-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("entry-join-" + UUID.randomUUID()));
        var objects = node.objects().server();
        objects.addEntrySpot(EntrySpot.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());

        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class,
                    runtime.actorManager()
                            .create(ACTOR_ID, "player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS));
            var context =
                    assertInstanceOf(
                            ZLinkActorRuntime.DefaultActorContext.class, PLAYER_CONTEXT.get());

            AtomicReference<String> submitted = new AtomicReference<>();
            ZLinkInternalSpotNode spotNode =
                    (ZLinkInternalSpotNode)
                            Proxy.newProxyInstance(
                                    ZLinkInternalSpotNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalSpotNode.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("joinActorEntrySpot")) {
                                            Message request = (Message) args[2];
                                            submitted.set(
                                                    StandardCharsets.UTF_8
                                                            .decode(request.dataBuffer())
                                                            .toString());
                                            return CompletableFuture.failedFuture(
                                                    new IllegalStateException("submitted"));
                                        }
                                        throw new UnsupportedOperationException(method.getName());
                                    });
            CompletableFuture<ZLinkActorRuntime.EntrySpotTarget> target = new CompletableFuture<>();
            CompletionStage<?> outcome =
                    new ZLinkActorEntrySpotJoinCall(
                                    context,
                                    timeout -> target,
                                    Message.from("entry-join-payload"),
                                    Duration.ofSeconds(5),
                                    new ZLinkActorEntrySpotJoinCall.Services(
                                            spotNode, null, null, null))
                            .execute();

            target.complete(
                    new ZLinkActorRuntime.EntrySpotTarget(
                            RoutingId.from("entry-target"), "entry-spot"));
            assertThrows(
                    Exception.class, () -> outcome.toCompletableFuture().get(5, TimeUnit.SECONDS));
            assertEquals("entry-join-payload", submitted.get());
        }
    }

    @Test
    void sameNodeJoinSelectsLocalAfterTargetAppearsDuringResolution() throws Exception {
        assertResolvedOwnerSnapshot(OwnerSnapshot.COMPLETE);
    }

    @Test
    void joinRejectsAnOwnerSnapshotWithoutANodeBeforeSelectingThePath() throws Exception {
        assertResolvedOwnerSnapshot(OwnerSnapshot.MISSING_NODE);
    }

    @Test
    void joinRejectsAnOwnerSnapshotWithoutAGenerationBeforeSelectingThePath() throws Exception {
        assertResolvedOwnerSnapshot(OwnerSnapshot.MISSING_GENERATION);
    }

    @Test
    void joinReturnsNotFoundWhenOwnerDisappearsAfterHandleResolution() throws Exception {
        assertResolvedOwnerSnapshot(OwnerSnapshot.DISAPPEARED);
    }

    private enum OwnerSnapshot {
        COMPLETE,
        MISSING_NODE,
        MISSING_GENERATION,
        DISAPPEARED
    }

    private void assertResolvedOwnerSnapshot(OwnerSnapshot snapshot) throws Exception {
        PLAYER_CONTEXT.set(null);
        resolvedRoom = null;
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        var store = new ZLinkInMemoryLocationStore();
        options.addLocationStore(store);
        var node = options.addRouteMesh("entry-join");
        node.listen("inproc://resolved-join-" + UUID.randomUUID())
                .setRoutingId(RoutingId.from("resolved-join"));
        var objects = node.objects().server();
        objects.addEntrySpot(EntrySpot.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.disableRelocation());
        objects.addSpotFactory(
                "resolved-room", ResolvedRoom.class, factory -> factory.disableRelocation());
        try (ZLinkFrameworkRuntime runtime =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options, new ZLinkJavaBackendAdapterFactory())) {
            runtime.actorManager()
                    .create(ACTOR_ID, "player")
                    .submit()
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
            var context =
                    assertInstanceOf(
                            ZLinkActorRuntime.DefaultActorContext.class, PLAYER_CONTEXT.get());
            var actors = (ZLinkActorRuntime) runtime.actorManager();
            try (var routes =
                    new ZLinkStoreLocationResolvers(
                            ZLinkRegisteredLocationStores.fromUnified(
                                    new ZLinkProviderLocationRepository(store)),
                            new ZLinkLocationOptions())) {
                var handles =
                        new ZLinkStoreSpotHandleResolver(
                                new ZLinkStoreLocationResolvers.AddressResolvers(
                                        List.of("entry-join"), routes));
                var resolutions = new AtomicInteger();
                class Resolver implements SpotHandleResolver, SpotTransportAddressResolver {
                    public CompletionStage<Optional<SpotHandle>> resolveSpotHandle(String spotId) {
                        resolutions.incrementAndGet();
                        return runtime.spotManager()
                                .getOrCreate(spotId, "resolved-room")
                                .submit()
                                .thenCompose(ignored -> handles.resolveSpotHandle(spotId));
                    }

                    public CompletionStage<Optional<SpotHandle>> resolveSpotHandle(
                            String mesh, String spotId) {
                        return resolveSpotHandle(spotId);
                    }

                    public CompletionStage<Optional<SpotTransportAddress>> resolve(String spotId) {
                        if (snapshot == OwnerSnapshot.DISAPPEARED) {
                            return CompletableFuture.completedFuture(Optional.empty());
                        }
                        return handles.resolve(spotId)
                                .thenApply(
                                        address ->
                                                address.map(
                                                        value ->
                                                                new SpotTransportAddress(
                                                                        value.routerChannelId(),
                                                                        snapshot
                                                                                        == OwnerSnapshot
                                                                                                .MISSING_NODE
                                                                                ? null
                                                                                : value
                                                                                        .targetNodeRid(),
                                                                        value.spotId(),
                                                                        snapshot
                                                                                        == OwnerSnapshot
                                                                                                .MISSING_GENERATION
                                                                                ? 0L
                                                                                : value
                                                                                        .spotGeneration(),
                                                                        value
                                                                                .targetNodeGeneration(),
                                                                        value
                                                                                .authorityOwnerGeneration(),
                                                                        value
                                                                                .ownerLeaseGeneration(),
                                                                        value.spotKind())));
                    }
                }
                var transports = new AtomicInteger();
                var canonical =
                        (ZLinkInternalMeshNode)
                                Proxy.newProxyInstance(
                                        ZLinkInternalMeshNode.class.getClassLoader(),
                                        new Class<?>[] {ZLinkInternalMeshNode.class},
                                        (proxy, method, args) -> {
                                            transports.incrementAndGet();
                                            throw new AssertionError(
                                                    "Same-node Join reached " + method.getName());
                                        });
                var entry =
                        (ZLinkBackendSpot)
                                Proxy.newProxyInstance(
                                        ZLinkBackendSpot.class.getClassLoader(),
                                        new Class<?>[] {ZLinkBackendSpot.class},
                                        (proxy, method, args) -> {
                                            if (method.getName().equals("rememberSpotAuthority"))
                                                return null;
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                        });
                var sourceNode =
                        (ZLinkInternalSpotNode)
                                Proxy.newProxyInstance(
                                        ZLinkInternalSpotNode.class.getClassLoader(),
                                        new Class<?>[] {ZLinkInternalSpotNode.class},
                                        (proxy, method, args) -> {
                                            if (method.getName().equals("entrySpot")) return entry;
                                            if (method.getName().equals("routingId"))
                                                return context.actorRef().nodeRid();
                                            throw new UnsupportedOperationException(
                                                    method.getName());
                                        });
                var services =
                        new ZLinkActorSpotJoinCall.Services(
                                sourceNode,
                                ignored -> resolvedRoom,
                                new Resolver(),
                                null,
                                (address, parts, timeout, internal) -> {
                                    transports.incrementAndGet();
                                    return CompletableFuture.failedFuture(
                                            new AssertionError("Fallback transport"));
                                },
                                canonical,
                                ignored -> "player",
                                null,
                                null,
                                actors,
                                null);
                var initialSpotId = context.spotId();
                var outcome =
                        new ZLinkActorSpotJoinCall(
                                        context,
                                        "resolved-room",
                                        Message.from(new byte[0]),
                                        Duration.ofSeconds(3),
                                        services)
                                .execute()
                                .toCompletableFuture();
                if (snapshot == OwnerSnapshot.COMPLETE) {
                    assertInstanceOf(
                            ZLinkActorJoinOutcome.Accepted.class, outcome.get(5, TimeUnit.SECONDS));
                    assertEquals("resolved-room", context.spotId().orElseThrow());
                } else if (snapshot == OwnerSnapshot.DISAPPEARED) {
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> outcome.get(5, TimeUnit.SECONDS));
                    var missing =
                            assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    failure.getCause());
                    assertEquals(
                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.NOT_FOUND,
                            missing.kind());
                    assertEquals(initialSpotId, context.spotId());
                } else {
                    var failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> outcome.get(5, TimeUnit.SECONDS));
                    var configuration =
                            assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkConfigurationException
                                            .class,
                                    failure.getCause());
                    assertEquals(
                            "SPOT remote address resolver returned an incomplete owner snapshot: resolved-room",
                            configuration.getMessage());
                    assertEquals(initialSpotId, context.spotId());
                }
                assertEquals(1, resolutions.get());
                assertEquals(0, transports.get());
            }
        }
    }

    private static volatile ResolvedRoom resolvedRoom;

    public static final class ResolvedRoom implements ZLinkSpot<Player> {
        private final ZLinkSpotContext context;

        public ResolvedRoom(ZLinkSpotContext context) {
            this.context = context;
            resolvedRoom = this;
        }

        public ZLinkSpotContext context() {
            return context;
        }

        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, systems.zlink.framework.messaging.ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }

        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
            PLAYER_CONTEXT.set(context);
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class EntrySpot implements ZLinkEntrySpot<Player> {
        private final ZLinkEntrySpotContext context;

        public EntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
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
}
