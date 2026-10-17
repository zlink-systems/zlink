package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.BooleanSupplier;

/** New-Actor creation candidates are the nodes whose Entry Spot type accepts the Actor type. */
final class ZLinkNewActorEntryCandidateTest {
    private static final String MESH = "entry-candidates";
    private static final int ACTORS = 24;

    private static final AtomicInteger FACTORY_ONLY_CREATES = new AtomicInteger();
    private static final AtomicInteger OTHER_ENTRY_CREATES = new AtomicInteger();
    private static final AtomicInteger ENTRY_CREATES = new AtomicInteger();
    private static final AtomicInteger STALE_CREATES = new AtomicInteger();

    @Test
    void newActorsAreCreatedOnlyOnNodesWhoseEntrySpotAcceptsTheActorType() throws Exception {
        FACTORY_ONLY_CREATES.set(0);
        OTHER_ENTRY_CREATES.set(0);
        ENTRY_CREATES.set(0);
        var store = new ZLinkInMemoryProviderLocationStore();

        try (var factoryOnly = server(store, "ec-factory-only", FactoryOnly.class, null);
                var otherEntry =
                        server(store, "ec-other-entry", OtherEntryFactory.class, OtherEntry.class);
                var entry = server(store, "ec-entry", EntryFactory.class, PlayerEntry.class)) {
            assertEquals(false, entryAccepted(factoryOnly));
            assertEquals(false, entryAccepted(otherEntry));
            assertEquals(true, entryAccepted(entry));

            try (var requester =
                    requester(
                            store,
                            factoryOnly.endpoint(),
                            otherEntry.endpoint(),
                            entry.endpoint())) {
                awaitAdmitted(requester, 3);
                createAll(requester, "ec");
            }
        }

        assertEquals(0, FACTORY_ONLY_CREATES.get());
        assertEquals(0, OTHER_ENTRY_CREATES.get());
        assertEquals(ACTORS, ENTRY_CREATES.get());
    }

    @Test
    void targetWithoutAcceptingEntrySpotRefusesBeforeFactoryAndRequesterPicksAnotherCandidate()
            throws Exception {
        STALE_CREATES.set(0);
        ENTRY_CREATES.set(0);
        var store = new ZLinkInMemoryProviderLocationStore();

        try (var stale = server(store, "st-stale", StaleFactory.class, PlayerEntry.class);
                var entry = server(store, "st-entry", EntryFactory.class, PlayerEntry.class)) {
            //  The published descriptor says entryAccepted=true; the target then loses its Entry
            //  Spot type, so only the target-side check can still refuse the creation.
            removeEntrySpots(stale.registration());
            assertEquals(false, stale.registration().acceptsNewActor("player"));

            try (var requester = requester(store, stale.endpoint(), entry.endpoint())) {
                awaitAdmitted(requester, 2);
                createAll(requester, "st");
            }
        }

        assertEquals(0, STALE_CREATES.get());
        assertEquals(ACTORS, ENTRY_CREATES.get());
    }

    @Test
    void readyActorIsFoundWithoutAnyNewActorCandidate() throws Exception {
        ENTRY_CREATES.set(0);
        var store = new ZLinkInMemoryProviderLocationStore();

        try (var entry = server(store, "ex-entry", EntryFactory.class, PlayerEntry.class);
                var requester = requester(store, entry.endpoint())) {
            awaitAdmitted(requester, 1);
            var created =
                    requester
                            .actorManager()
                            .getOrCreate("ex-player", "player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
            assertInstanceOf(ZLinkActorCreateResult.Created.class, created);

            //  With the only Entry node no longer an admitted peer, no new-Actor candidate
            //  remains, yet the Ready Actor is still found.
            var node = requester.monitoringMeshNode(MESH);
            node.removePeerConnection(node.connectionIntentIds().getFirst());
            await(
                    () ->
                            node.peers().stream()
                                    .noneMatch(peer -> peer.state() == MeshPeerState.ADMITTED));
            var existing =
                    requester
                            .actorManager()
                            .getOrCreate("ex-player", "player")
                            .submit()
                            .toCompletableFuture()
                            .get(10, TimeUnit.SECONDS);
            assertInstanceOf(ZLinkActorCreateResult.Existing.class, existing);
        }
        assertEquals(1, ENTRY_CREATES.get());
    }

    private static void createAll(ZLinkFrameworkRuntime requester, String prefix) throws Exception {
        List<CompletableFuture<ZLinkActorCreateResult>> results = new ArrayList<>();
        for (int index = 0; index < ACTORS; index++) {
            results.add(
                    requester
                            .actorManager()
                            .create(prefix + "-player-" + index, "player")
                            .submit()
                            .toCompletableFuture());
        }
        for (var result : results) {
            assertInstanceOf(
                    ZLinkActorCreateResult.Created.class, result.get(20, TimeUnit.SECONDS));
        }
    }

    private static boolean entryAccepted(Server server) {
        return server.runtime().monitoringMeshNodeProjection(MESH).objectCapabilities().stream()
                .filter(
                        capability ->
                                capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
                                        && capability.stableType().equals("player"))
                .findFirst()
                .orElseThrow()
                .entryAccepted();
    }

    @SuppressWarnings("unchecked")
    private static void removeEntrySpots(MeshNodeRegistration registration) throws Exception {
        Field field = MeshNodeRegistration.class.getDeclaredField("entrySpots");
        field.setAccessible(true);
        ((List<Object>) field.get(registration)).clear();
    }

    private static Server server(
            ZLinkInMemoryProviderLocationStore store,
            String rid,
            Class<? extends ZLinkActorFactory> factory,
            Class<? extends ZLinkEntrySpot<?>> entry)
            throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from(rid));
        var objects = mesh.objects().server();
        objects.addActorFactory("player", Player.class, factory, f -> f.disableRelocation());
        if (entry != null) {
            objects.addEntrySpot(entry);
        }
        if (entry == OtherEntry.class) {
            objects.addActorFactory(
                    "other", Other.class, OtherEntryFactory.class, f -> f.disableRelocation());
        }
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
        await(runtime::isReady);
        MeshNodeRegistration registration =
                options.registration().meshNodes().stream()
                        .filter(node -> node.meshName().equals(MESH))
                        .findFirst()
                        .orElseThrow();
        return new Server(runtime, registration);
    }

    private static ZLinkFrameworkRuntime requester(
            ZLinkInMemoryProviderLocationStore store, String... endpoints) throws Exception {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        var mesh =
                options.addRouteMesh(MESH)
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from("ec-requester"));
        mesh.objects().client();
        for (String endpoint : endpoints) {
            mesh.peerConnections().connect(endpoint);
        }
        var runtime = ZLinkFrameworkRuntimeTestAccess.start(options);
        await(runtime::isReady);
        return runtime;
    }

    private static void awaitAdmitted(ZLinkFrameworkRuntime requester, int count) throws Exception {
        var node = requester.monitoringMeshNode(MESH);
        await(
                () ->
                        node.peers().stream()
                                        .filter(peer -> peer.state() == MeshPeerState.ADMITTED)
                                        .count()
                                == count);
    }

    private static void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            TimeUnit.MILLISECONDS.sleep(10);
        }
        assertTrue(condition.getAsBoolean(), "condition was not met in time");
    }

    private record Server(ZLinkFrameworkRuntime runtime, MeshNodeRegistration registration)
            implements AutoCloseable {
        String endpoint() {
            return runtime.monitoringMeshNode(MESH).status().localEndpoint();
        }

        @Override
        public void close() {
            runtime.close();
        }
    }

    public static class Player implements ZLinkActor {
        private final ZLinkActorContext context;

        public Player(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Other extends Player {
        public Other(ZLinkActorContext context) {
            super(context);
        }
    }

    public abstract static class CountingFactory implements ZLinkActorFactory {
        abstract AtomicInteger counter();

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            counter().incrementAndGet();
            return CompletableFuture.completedFuture(new Player(context));
        }
    }

    public static final class FactoryOnly extends CountingFactory {
        @Override
        AtomicInteger counter() {
            return FACTORY_ONLY_CREATES;
        }
    }

    public static final class OtherEntryFactory extends CountingFactory {
        @Override
        AtomicInteger counter() {
            return OTHER_ENTRY_CREATES;
        }
    }

    public static final class EntryFactory extends CountingFactory {
        @Override
        AtomicInteger counter() {
            return ENTRY_CREATES;
        }
    }

    public static final class StaleFactory extends CountingFactory {
        @Override
        AtomicInteger counter() {
            return STALE_CREATES;
        }
    }

    public abstract static class AcceptingEntry<TActor extends ZLinkActor>
            implements ZLinkEntrySpot<TActor> {
        private final ZLinkEntrySpotContext context;

        protected AcceptingEntry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<ZLinkActorCreateResponse> onCreateActor(
                TActor actor, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerEntry extends AcceptingEntry<Player> {
        public PlayerEntry(ZLinkEntrySpotContext context) {
            super(context);
        }
    }

    public static final class OtherEntry extends AcceptingEntry<Other> {
        public OtherEntry(ZLinkEntrySpotContext context) {
            super(context);
        }
    }
}
