package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMutation;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.spots.ZLinkSpotKind;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;

final class ZLinkLocationLifecycleTest {
    @Test
    void localActorOwnershipTracksClaimReferenceAndRelease() {
        var store = new ZLinkInMemoryLocationStore();
        try (var runtime =
                        new ZLinkLocationRuntime(
                                store, "owner-a", Duration.ofSeconds(30), Duration.ofSeconds(5));
                var lifecycle = new ZLinkLocationLifecycle(runtime)) {
            RoutingId node = RoutingId.from("node-a");

            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    lifecycle
                            .claimActor("player", "actor-a", node, () -> {})
                            .toCompletableFuture()
                            .join());
            assertTrue(lifecycle.ownsActor("player", "actor-a"));
            lifecycle
                    .setActorRef("player", "actor-a", new ActorRef("actor-a", 7, "game", node))
                    .toCompletableFuture()
                    .join();
            lifecycle.releaseActor("player", "actor-a").toCompletableFuture().join();
            assertFalse(lifecycle.ownsActor("player", "actor-a"));
        }
    }

    @Test
    void closeClearsTrackedSpotAndActorMaterializations() {
        var store = new ZLinkInMemoryLocationStore();
        var runtime =
                new ZLinkLocationRuntime(
                        store, "owner-a", Duration.ofSeconds(30), Duration.ofSeconds(5));
        var lifecycle = new ZLinkLocationLifecycle(runtime);
        RoutingId node = RoutingId.from("node-a");
        lifecycle
                .claimSpot("game", "room-a", 3, "room", node, ZLinkSpotKind.USER, null, () -> {})
                .toCompletableFuture()
                .join();
        lifecycle.claimActor("player", "actor-a", node, () -> {}).toCompletableFuture().join();

        lifecycle.close();

        assertFalse(lifecycle.ownsActor("player", "actor-a"));
        runtime.close();
    }

    @Test
    void actorJoinUpdatesDurableAuthorityWithStoreVersionCas() {
        RoutingId node = RoutingId.from("node-a");
        String actorId = "actor-join";
        String spotId = "room-a";
        var store = new ActorJoinStore(node, actorId, spotId);
        var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(30),
                        Duration.ofSeconds(5));
        var lifecycle = new ZLinkLocationLifecycle(runtime);
        lifecycle.claimActor("player", actorId, node, () -> {}).toCompletableFuture().join();
        lifecycle
                .setActorRef("player", actorId, new ActorRef(actorId, 7, "game", node))
                .toCompletableFuture()
                .join();

        lifecycle
                .notifyActorJoinedSpot("player", actorId, "game", spotId)
                .toCompletableFuture()
                .join();

        var authority =
                new ZLinkActorAuthorityPayloadCodec()
                        .decode(store.row(ZLinkAuthorityKeyCodec.actor(actorId)).payload())
                        .orElseThrow();
        assertEquals(spotId, authority.currentSpotId());
        assertEquals(2, authority.currentSpotKind());
        assertEquals(11, authority.currentSpotGeneration());
        assertEquals("game", authority.meshName());
        assertEquals(node, authority.nodeRid());
        assertEquals(1, store.casCount);
        lifecycle.close();
        runtime.close();
    }

    @Test
    void actorJoinDoesNotPublishWhenTheAuthorityCasLoses() {
        RoutingId node = RoutingId.from("node-a");
        String actorId = "actor-cas-loser";
        String spotId = "room-a";
        var store = new ActorJoinStore(node, actorId, spotId, true);
        var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(30),
                        Duration.ofSeconds(5));
        var lifecycle = new ZLinkLocationLifecycle(runtime);
        lifecycle.claimActor("player", actorId, node, () -> {}).toCompletableFuture().join();
        lifecycle
                .setActorRef("player", actorId, new ActorRef(actorId, 7, "game", node))
                .toCompletableFuture()
                .join();

        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                lifecycle
                                        .notifyActorJoinedSpot("player", actorId, "game", spotId)
                                        .toCompletableFuture()
                                        .join());

        assertEquals(
                "Actor Spot join authority CAS conflicted: " + actorId,
                failure.getCause().getMessage());
        assertEquals(0, store.casCount);
        var authority =
                new ZLinkActorAuthorityPayloadCodec()
                        .decode(store.row(ZLinkAuthorityKeyCodec.actor(actorId)).payload())
                        .orElseThrow();
        assertEquals("entry-a", authority.currentSpotId());
        lifecycle.close();
        runtime.close();
    }

    @Test
    void actorJoinRejectsAStaleLocalActorGenerationBeforeCas() {
        RoutingId node = RoutingId.from("node-a");
        String actorId = "actor-stale-generation";
        String spotId = "room-a";
        var store = new ActorJoinStore(node, actorId, spotId);
        var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(store),
                        "owner-a",
                        Duration.ofSeconds(30),
                        Duration.ofSeconds(5));
        var lifecycle = new ZLinkLocationLifecycle(runtime);
        lifecycle.claimActor("player", actorId, node, () -> {}).toCompletableFuture().join();
        lifecycle
                .setActorRef("player", actorId, new ActorRef(actorId, 8, "game", node))
                .toCompletableFuture()
                .join();

        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                lifecycle
                                        .notifyActorJoinedSpot("player", actorId, "game", spotId)
                                        .toCompletableFuture()
                                        .join());

        assertEquals(
                "Actor authority changed before durable Spot join: " + actorId,
                failure.getCause().getMessage());
        assertEquals(0, store.casCount);
        lifecycle.close();
        runtime.close();
    }

    @ParameterizedTest
    @CsvSource({
        "missing,NOT_FOUND",
        "fence,UNAVAILABLE",
        "invalid,INTERNAL_FAILURE",
        "cas,UNAVAILABLE",
        "typed-notfound,NOT_FOUND",
        "typed-unavailable,UNAVAILABLE",
        "exhausted,INTERNAL_FAILURE",
        "store-error,INTERNAL_FAILURE"
    })
    void joinFailureKeepsMembershipClassification(
            String condition, ZLinkFrameworkErrorKind expected) {
        var node = RoutingId.from("node-a");
        var store =
                new ActorJoinStore(
                        node,
                        "kind-actor",
                        "room-a",
                        condition.equals("cas") || condition.startsWith("typed-"));
        store.writeFailure = condition;
        var spotKey = ZLinkAuthorityKeyCodec.spot("room-a");
        if (condition.startsWith("typed-")) {
            store.membershipFailure =
                    condition.equals("typed-notfound")
                            ? ZLinkFrameworkErrorKind.NOT_FOUND
                            : ZLinkFrameworkErrorKind.UNAVAILABLE;
        }
        if (condition.equals("missing")) store.rows.remove(spotKey);
        if (condition.equals("fence") || condition.equals("invalid")) {
            byte[] payload =
                    condition.equals("invalid")
                            ? new byte[] {0}
                            : new ZLinkServiceAuthorityPayloadCodec()
                                    .encodeUser(
                                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                                            "room",
                                            "room-a",
                                            "owner-a",
                                            1,
                                            "other-mesh",
                                            node,
                                            8);
            store.rows.put(
                    spotKey,
                    store.snapshot(
                            "spot-changed",
                            payload,
                            11,
                            ZLinkPlacementObjectKind.USER_SPOT,
                            "room"));
        }
        try (var runtime =
                        new ZLinkLocationRuntime(
                                ZLinkRegisteredLocationStores.fromUnified(store),
                                "owner-a",
                                Duration.ofSeconds(30),
                                Duration.ofSeconds(5));
                var lifecycle = new ZLinkLocationLifecycle(runtime)) {
            lifecycle
                    .setActorRef(
                            "player", "kind-actor", new ActorRef("kind-actor", 7, "game", node))
                    .toCompletableFuture()
                    .join();
            var failure =
                    assertThrows(
                                    CompletionException.class,
                                    () ->
                                            lifecycle
                                                    .notifyActorJoinedSpot(
                                                            "player",
                                                            "kind-actor",
                                                            "game",
                                                            "room-a")
                                                    .toCompletableFuture()
                                                    .join())
                            .getCause();
            assertEquals(
                    expected,
                    failure instanceof ZLinkFrameworkException framework
                            ? framework.kind()
                            : ZLinkFrameworkErrorKind.INTERNAL_FAILURE);
            assertEquals(0, store.casCount);
        }
    }

    private static final class ActorJoinStore extends ZLinkLocationStoreTestAdapter {
        private final Map<String, ZLinkAuthoritySnapshot> rows = new ConcurrentHashMap<>();
        private final RoutingId node;
        private final String actorId;
        private final String spotId;
        private final boolean forceConflict;
        private int casCount;
        private ZLinkFrameworkErrorKind membershipFailure;
        private String writeFailure;

        ActorJoinStore(RoutingId node, String actorId, String spotId) {
            this(node, actorId, spotId, false);
        }

        ActorJoinStore(RoutingId node, String actorId, String spotId, boolean forceConflict) {
            this.node = node;
            this.actorId = actorId;
            this.spotId = spotId;
            this.forceConflict = forceConflict;
            byte[] actor =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "player",
                                    actorId,
                                    "entry-a",
                                    3,
                                    1,
                                    "owner-a",
                                    1,
                                    "game",
                                    node,
                                    8);
            byte[] spot =
                    new ZLinkServiceAuthorityPayloadCodec()
                            .encodeUser(
                                    ZLinkServiceAuthorityPayloadCodec.State.READY,
                                    "room",
                                    spotId,
                                    "owner-a",
                                    1,
                                    "game",
                                    node,
                                    9);
            rows.put(
                    ZLinkAuthorityKeyCodec.actor(actorId),
                    snapshot("actor-v1", actor, 7, ZLinkPlacementObjectKind.ACTOR, "player"));
            rows.put(
                    ZLinkAuthorityKeyCodec.spot(spotId),
                    snapshot("spot-v1", spot, 11, ZLinkPlacementObjectKind.USER_SPOT, "room"));
        }

        ZLinkAuthoritySnapshot row(String key) {
            return rows.get(key);
        }

        @Override
        public CompletionStage<ZLinkAuthorityReadResult> read(
                String key, ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            return CompletableFuture.completedFuture(
                    current == null ? new ZLinkAuthorityMissing(Instant.now()) : current);
        }

        @Override
        public CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
                String key,
                ZLinkAuthorityExpectation expectation,
                ZLinkAuthorityMutation mutation,
                ZLinkStoreCancellation cancellation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            if ("exhausted".equals(writeFailure))
                return CompletableFuture.completedFuture(new ZLinkAuthorityGenerationExhausted());
            if ("store-error".equals(writeFailure))
                return CompletableFuture.failedFuture(
                        new IllegalStateException("store internal failure"));
            if (forceConflict) {
                if (membershipFailure == null)
                    rows.put(
                            key,
                            new ZLinkAuthoritySnapshot(
                                    "actor-concurrent",
                                    current.payload(),
                                    current.objectGeneration(),
                                    current.authorityOwnerGeneration(),
                                    current.ownerId(),
                                    current.ownerLeaseGeneration(),
                                    current.allocation(),
                                    current.storeNow()));
                return CompletableFuture.completedFuture(
                        new ZLinkAuthorityConflict(current, membershipFailure));
            }
            if (!(current != null
                    && expectation instanceof ZLinkAuthorityExpectFound expected
                    && mutation instanceof ZLinkAuthorityPut put
                    && current.storeVersion().equals(expected.storeVersion()))) {
                return CompletableFuture.completedFuture(
                        new ZLinkAuthorityConflict(
                                current == null
                                        ? new ZLinkAuthorityMissing(Instant.now())
                                        : current));
            }
            casCount++;
            ZLinkAuthoritySnapshot next =
                    new ZLinkAuthoritySnapshot(
                            "actor-v2",
                            put.payload(),
                            current.objectGeneration(),
                            current.authorityOwnerGeneration(),
                            current.ownerId(),
                            current.ownerLeaseGeneration(),
                            current.allocation(),
                            Instant.now());
            rows.put(key, next);
            return CompletableFuture.completedFuture(
                    new ZLinkAuthorityStored(
                            next.storeVersion(),
                            next.payload(),
                            next.objectGeneration(),
                            next.authorityOwnerGeneration(),
                            next.ownerId(),
                            next.ownerLeaseGeneration(),
                            next.allocation(),
                            next.storeNow()));
        }

        private ZLinkAuthoritySnapshot snapshot(
                String version,
                byte[] payload,
                long generation,
                ZLinkPlacementObjectKind kind,
                String stableType) {
            return new ZLinkAuthoritySnapshot(
                    version,
                    payload,
                    generation,
                    1,
                    "owner-a",
                    1,
                    new ZLinkPlacementAllocation(
                            ZLinkPlacementAllocationState.ACTIVE,
                            kind,
                            stableType,
                            new ZLinkMeshNodeDescriptorKey("game", node),
                            8,
                            kind == ZLinkPlacementObjectKind.ACTOR
                                    ? ZLinkPlacementCapacityBundle.actor(1)
                                    : ZLinkPlacementCapacityBundle.spot(kind, stableType, 1)),
                    Instant.now());
        }
    }
}
