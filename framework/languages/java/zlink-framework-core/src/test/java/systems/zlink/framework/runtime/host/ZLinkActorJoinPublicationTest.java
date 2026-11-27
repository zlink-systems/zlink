package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.parallel.Execution;
import org.junit.jupiter.api.parallel.ExecutionMode;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.ZLinkRequestStage;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkProviderLocationRepository;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.*;

import java.io.IOException;
import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

@Execution(ExecutionMode.SAME_THREAD)
final class ZLinkActorJoinPublicationTest {
    private static final String MESH = "join-publication";
    private static final String ACTOR = "publication-player";
    private static final String ROOM = "publication-room";
    private static final RoutingId SOURCE = RoutingId.from("publication-source");
    private static final RoutingId TARGET = RoutingId.from("publication-target");
    private static final Duration WAIT = Duration.ofSeconds(10);
    private static volatile Scenario scenario;

    @ParameterizedTest(name = "{0}, accepted requests: {1}")
    @CsvSource({"remote, 0", "remote, 1", "local, 0", "local, 1"})
    void publishesJoinBeforeRunningAcceptedRequests(String placement, int acceptedRequests)
            throws Exception {
        boolean remote = placement.equals("remote");
        var observations = new Scenario();
        scenario = observations;
        var relocations = new InMemoryRelocationStore();
        try (var source =
                ZLinkFrameworkRuntimeTestAccess.start(
                        options(SOURCE, observations.store, relocations, null, !remote))) {
            awaitReady(source);
            var endpoint = source.listenerStatus(ZLinkListenerKind.ROUTE_MESH, MESH).endpoint();
            try (var peer =
                            remote
                                    ? ZLinkFrameworkRuntimeTestAccess.start(
                                            options(
                                                    TARGET,
                                                    observations.store,
                                                    relocations,
                                                    endpoint,
                                                    true))
                                    : null;
                    AutoCloseable release = observations::release) {
                var target = remote ? peer : source;
                if (remote) {
                    awaitReady(target);
                    try (var sourceReady =
                                    ZLinkRelocationObservationPublicTest.observe(
                                            source.routeMeshRuntime().observe(MESH, 8),
                                            status ->
                                                    status.isReady()
                                                            && status.readyPeerCount() > 0);
                            var targetReady =
                                    ZLinkRelocationObservationPublicTest.observe(
                                            target.routeMeshRuntime().observe(MESH, 8),
                                            status ->
                                                    status.isReady()
                                                            && status.readyPeerCount() > 0)) {
                        await(sourceReady.result);
                        await(targetReady.result);
                    }
                }
                var expectedOwner = remote ? TARGET : SOURCE;
                assertEquals(
                        expectedOwner,
                        await(target.spotManager().getOrCreate(ROOM, "room").submit())
                                .spot()
                                .nodeRid());
                assertInstanceOf(
                        ZLinkActorCreateResult.Created.class,
                        await(source.actorManager().create(ACTOR, "player").submit()));

                var client = source.actorClient();
                await(client.sendToActor(ACTOR, new Move()).submit());
                await(observations.moveEntered);
                ZLinkRequestStage<Integer> request = null;
                if (acceptedRequests == 1) {
                    request =
                            client.requestToActor(ACTOR, new Probe(71))
                                    .timeout(WAIT)
                                    .submit(Integer.class);
                    await(request.admitted());
                    assertFalse(
                            request.toCompletableFuture().isDone(),
                            "the accepted request must still be queued on the source");
                }
                observations.moveRelease.complete(null);

                var callback = await(observations.callback);
                assertCallback(callback, expectedOwner);
                if (remote) {
                    assertPublicationPending(callback.authority());
                } else {
                    assertTrue(callback.authority().steadyReady());
                }
                assertFalse(
                        observations.handlerEntered.isDone(),
                        "the accepted request must wait for the Join callback");
                assertFalse(
                        observations.store.normalizing.isDone(),
                        "authority normalization must follow the Join callback");
                observations.callbackRelease.complete(null);

                if (request != null) {
                    await(observations.handlerEntered);
                    assertFalse(request.toCompletableFuture().isDone());
                }
                if (remote) {
                    await(observations.store.normalizing);
                    assertPublicationPending(await(observations.authority()));
                    observations.store.normalizationRelease.complete(null);
                    await(observations.store.normalized);
                } else {
                    assertFalse(observations.store.normalizing.isDone());
                }
                assertTrue(await(observations.authority()).steadyReady());

                if (request != null) {
                    assertFalse(
                            request.toCompletableFuture().isDone(),
                            "authority must be steady while the accepted request is still running");
                    observations.handlerRelease.complete(null);
                    assertEquals(71, await(request));
                }
            }
        }
    }

    private static void assertCallback(Callback callback, RoutingId expectedOwner) {
        assertInstanceOf(ZLinkActorJoinCompletion.Accepted.class, callback.completion());
        assertEquals(expectedOwner, callback.authority().allocation().descriptor().rid());
    }

    private static void assertPublicationPending(ZLinkAuthoritySnapshot authority) {
        assertNotNull(authority.relocation());
        assertTrue(authority.relocation().targetPublicationPending());
    }

    private static void awaitReady(ZLinkFrameworkRuntime runtime) throws Exception {
        await(ZLinkFrameworkRuntimeTestAccess.startupCompletion(runtime));
        if (!runtime.isReady()) {
            assertTrue(await(ZLinkFrameworkRuntimeTestAccess.renewOwnerLeaseOnce(runtime)));
        }
        assertTrue(runtime.isReady());
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(WAIT.toSeconds(), TimeUnit.SECONDS);
    }

    private static DefaultZLinkFrameworkOptions options(
            RoutingId rid,
            ZLinkLocationStore locations,
            InMemoryRelocationStore relocations,
            String peer,
            boolean hostsRoom) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureLocations().setOwnerLeaseFencingMargin(Duration.ofNanos(1));
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh(MESH).listen("tcp://127.0.0.1:0").setRoutingId(rid);
        if (peer != null) node.peerConnections().connect(peer);
        var objects = node.objects().server();
        objects.addEntrySpot(Entry.class);
        objects.addActorFactory(
                "player",
                Player.class,
                PlayerFactory.class,
                factory -> factory.recreateOnRelocation());
        if (hostsRoom) {
            objects.addSpotFactory("room", Room.class, factory -> factory.recreateOnRelocation());
        }
        return options;
    }

    private record Callback(
            ZLinkActorJoinCompletion completion, ZLinkAuthoritySnapshot authority) {}

    private static final class Scenario {
        final NormalizationStore store = new NormalizationStore();
        final ZLinkProviderLocationRepository repository =
                new ZLinkProviderLocationRepository(store);
        final CompletableFuture<Void> moveEntered = new CompletableFuture<>();
        final CompletableFuture<Void> moveRelease = new CompletableFuture<>();
        final CompletableFuture<Callback> callback = new CompletableFuture<>();
        final CompletableFuture<Void> callbackRelease = new CompletableFuture<>();
        final CompletableFuture<Void> handlerEntered = new CompletableFuture<>();
        final CompletableFuture<Void> handlerRelease = new CompletableFuture<>();

        CompletionStage<ZLinkAuthoritySnapshot> authority() {
            return repository
                    .read(ZLinkAuthorityKeyCodec.actor(ACTOR), () -> false)
                    .thenApply(ZLinkAuthoritySnapshot.class::cast);
        }

        void release() {
            moveRelease.complete(null);
            callbackRelease.complete(null);
            handlerRelease.complete(null);
            store.normalizationRelease.complete(null);
        }
    }

    private static final class NormalizationStore implements ZLinkLocationStore {
        private static final ZLinkStoreKey ACTOR_ROW =
                new ZLinkStoreKey("authority\0actor\0" + ACTOR);
        private static final ObjectMapper JSON = new ObjectMapper();

        private final ZLinkLocationStore inner = new ZLinkInMemoryLocationStore();
        final CompletableFuture<Void> normalizing = new CompletableFuture<>();
        final CompletableFuture<Void> normalizationRelease = new CompletableFuture<>();
        final CompletableFuture<Void> normalized = new CompletableFuture<>();

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return inner.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            for (var mutation : request.mutations()) {
                if (mutation instanceof ZLinkStorePut put
                        && put.key().equals(ACTOR_ROW)
                        && !hasRelocation(put.bytes())
                        && request.conditions().stream()
                                .anyMatch(
                                        condition ->
                                                condition
                                                                instanceof
                                                                ZLinkStoreVersionCondition version
                                                        && version.key().equals(ACTOR_ROW))) {
                    return inner.read(ACTOR_ROW, cancellation)
                            .thenCompose(
                                    read -> {
                                        if (read instanceof ZLinkStoreReadFound found
                                                && hasRelocation(found.value().bytes())) {
                                            normalizing.complete(null);
                                            return normalizationRelease
                                                    .thenCompose(
                                                            ignored ->
                                                                    inner.write(
                                                                            request, cancellation))
                                                    .thenApply(
                                                            result -> {
                                                                if (result
                                                                        instanceof
                                                                        ZLinkStoreWriteApplied)
                                                                    normalized.complete(null);
                                                                return result;
                                                            });
                                        }
                                        return inner.write(request, cancellation);
                                    });
                }
            }
            return inner.write(request, cancellation);
        }

        private static boolean hasRelocation(byte[] bytes) {
            try {
                return JSON.readTree(bytes).path("aggregate").hasNonNull("relocation");
            } catch (IOException failure) {
                throw new IllegalArgumentException("Invalid Actor authority record", failure);
            }
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return inner.scan(request, cancellation);
        }
    }

    public record Move() {}

    public record Probe(int value) {}

    public record Player(ZLinkActorContext context, Scenario observations) implements ZLinkActor {
        @Override
        public CompletionStage<Void> onJoinCompleted(ZLinkActorJoinCompletion result) {
            observations
                    .authority()
                    .thenApply(authority -> new Callback(result, authority))
                    .whenComplete(
                            (callback, failure) -> {
                                if (failure == null) observations.callback.complete(callback);
                                else observations.callback.completeExceptionally(failure);
                            });
            return observations.callback.thenCompose(ignored -> observations.callbackRelease);
        }
    }

    public static final class PlayerFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new Player(context, scenario));
        }
    }

    public record Entry(ZLinkEntrySpotContext context) implements ZLinkEntrySpot<Player> {
        @Override
        public void configure() {
            context.handlers().addHandler(MoveHandler.class);
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

    public record Room(ZLinkSpotContext context) implements ZLinkSpot<Player> {
        @Override
        public void configure() {
            context.handlers().addHandler(ProbeHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(Player actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept());
        }
    }

    public static final class MoveHandler
            implements ZLinkEntrySpotActorSendHandler<Entry, Player, Move> {
        @Override
        public CompletionStage<Void> handle(
                Entry entry, Player actor, ZLinkMessageContext context, Move move) {
            actor.context().joinSpot(ROOM).defer();
            actor.observations().moveEntered.complete(null);
            return actor.observations().moveRelease;
        }
    }

    public static final class ProbeHandler
            implements ZLinkSpotActorRequestHandler<Room, Player, Probe, Integer> {
        @Override
        public CompletionStage<Integer> handle(
                Room room, Player actor, ZLinkMessageContext context, Probe probe) {
            var observations = actor.observations();
            observations.handlerEntered.complete(null);
            return observations.handlerRelease.thenApply(ignored -> probe.value());
        }
    }
}
