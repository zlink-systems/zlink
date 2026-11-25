package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode.PeerAuthorityFence;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.*;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkTargetEarlyAdmissionTest {
    private static final RoutingId SOURCE = RoutingId.from("source");
    private static final RoutingId TARGET = RoutingId.from("target");
    private static final RoutingId PEER = RoutingId.from("peer");
    private static final String ACTOR = "actor-a";
    private static final String SPOT = "room-a";
    private static final String ACTOR_KEY = ZLinkAuthorityKeyCodec.actor(ACTOR);
    private static final String SPOT_KEY = ZLinkAuthorityKeyCodec.spot(SPOT);

    @Test
    void localActorRequestsReplayInOrder() throws Exception {
        verifyActorRequestOrder(new PeerAuthorityFence(TARGET, 17, "target-owner", 23));
    }

    @Test
    void remoteActorRequestsReplayInOrder() throws Exception {
        verifyActorRequestOrder(new PeerAuthorityFence(PEER, 21, "peer-owner", 31));
    }

    private void verifyActorRequestOrder(PeerAuthorityFence source) throws Exception {
        var fixture = new Fixture();
        var first = fixture.actor(source, 1);
        var second = fixture.actor(source, 2);
        assertFalse(first.isDone());
        assertFalse(second.isDone());
        fixture.open();
        assertEquals(1, first.get(5, TimeUnit.SECONDS));
        assertEquals(2, second.get(5, TimeUnit.SECONDS));
        assertEquals(List.of("actor-1", "actor-2"), fixture.executed);
    }

    @Test
    void spotRequestsReplayInOrderAndRejectWrongGeneration() throws Exception {
        var fixture = new Fixture();
        var first = fixture.spot(6, 1);
        var second = fixture.spot(6, 2);
        var stale = fixture.spot(9, 3);
        assertFalse(first.isDone());
        assertFalse(second.isDone());
        var rejection = assertThrows(java.util.concurrent.CompletionException.class, stale::join);
        assertArrayEquals(
                new int[] {
                    RequestResult.CONFLICT.value(),
                    (int) ServiceWireConstants.FRAMEWORK_ERROR_SPOT_GENERATION_STALE
                },
                ZLinkRequestFailureMapping.outgoing(rejection));
        fixture.open();
        assertEquals(1, first.get(5, TimeUnit.SECONDS));
        assertEquals(2, second.get(5, TimeUnit.SECONDS));
        assertEquals(List.of("spot-1", "spot-2"), fixture.executed);
    }

    @Test
    void pendingActorAuthorityCheckDoesNotBlockOtherObjectIngress() throws Exception {
        var fixture = new Fixture();
        fixture.blockedKey.set(ACTOR_KEY);
        var first = fixture.actor(new PeerAuthorityFence(TARGET, 17, "target-owner", 23), 1);
        try {
            fixture.readStarted.get(5, TimeUnit.SECONDS);
            var spot = fixture.spot(6, 1);
            assertTrue(fixture.spotRead.isDone());
            var second = fixture.actor(new PeerAuthorityFence(PEER, 21, "peer-owner", 31), 2);
            assertFalse(first.isDone());
            assertFalse(second.isDone());
            assertFalse(spot.isDone());
            var opening = fixture.endpoint.publish(fixture.request).toCompletableFuture();
            assertFalse(opening.isDone());
            assertTrue(fixture.executed.isEmpty());
            fixture.releaseRead.complete(null);
            opening.get(5, TimeUnit.SECONDS);
            assertEquals(1, spot.get(5, TimeUnit.SECONDS));
            assertEquals(1, first.get(5, TimeUnit.SECONDS));
            assertEquals(2, second.get(5, TimeUnit.SECONDS));
            assertEquals(
                    List.of("actor-1", "actor-2"),
                    fixture.executed.stream().filter(value -> value.startsWith("actor-")).toList());
        } finally {
            fixture.releaseRead.complete(null);
        }
    }

    @Test
    void pendingSpotAuthorityCheckDoesNotBlockActorIngress() throws Exception {
        var fixture = new Fixture();
        fixture.blockedKey.set(SPOT_KEY);
        var spot = fixture.spot(6, 1);
        try {
            fixture.readStarted.get(5, TimeUnit.SECONDS);
            var actor = fixture.actor(new PeerAuthorityFence(PEER, 21, "peer-owner", 31), 1);
            assertTrue(fixture.actorRead.isDone());
            assertFalse(actor.isDone());
            assertFalse(spot.isDone());
            var opening = fixture.endpoint.publish(fixture.request).toCompletableFuture();
            assertFalse(opening.isDone());
            fixture.releaseRead.complete(null);
            opening.get(5, TimeUnit.SECONDS);
            assertEquals(1, spot.get(5, TimeUnit.SECONDS));
            assertEquals(1, actor.get(5, TimeUnit.SECONDS));
        } finally {
            fixture.releaseRead.complete(null);
        }
    }

    private static final class Fixture {
        final List<String> executed = new ArrayList<>();
        final AtomicReference<String> blockedKey = new AtomicReference<>();
        final CompletableFuture<Void> readStarted = new CompletableFuture<>();
        final CompletableFuture<Void> releaseRead = new CompletableFuture<>();
        final AtomicBoolean committed = new AtomicBoolean();
        final CompletableFuture<Void> spotRead = new CompletableFuture<>();
        final CompletableFuture<Void> actorRead = new CompletableFuture<>();
        final ZLinkUserSpotRetireTargetEndpoint endpoint;
        final ZLinkSpotRetireControl.StageRequest request;

        Fixture() {
            var authority = new AuthorityState();
            var store =
                    authority.proxy(
                            (key, value) -> {
                                if (key.equals(blockedKey.get())) {
                                    blockedKey.set(null);
                                    readStarted.complete(null);
                                    return releaseRead.thenApply(ignored -> value);
                                }
                                if (key.equals(SPOT_KEY) && committed.get())
                                    spotRead.complete(null);
                                if (key.equals(ACTOR_KEY) && committed.get()) {
                                    actorRead.complete(null);
                                }
                                return CompletableFuture.completedFuture(value);
                            });
            var coordinator = new ZLinkAggregateRelocationCoordinator(store);
            var participants =
                    List.of(
                            new ZLinkSpotRetireControl.ParticipantFence(
                                    ACTOR_KEY, 1, ACTOR, "player", false, 4, 7),
                            new ZLinkSpotRetireControl.ParticipantFence(
                                    SPOT_KEY, 2, SPOT, "room", false, 3, 5));
            var id = UUID.randomUUID();
            var envelope =
                    new ZLinkUserSpotAggregateStagingOwner.Request(
                            TestSpot.class,
                            "room",
                            SPOT,
                            3,
                            new byte[0],
                            false,
                            new byte[0],
                            List.of(
                                    new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                            ACTOR,
                                            "player",
                                            new byte[0],
                                            false,
                                            new ZLinkBackendActorRef(TARGET, ACTOR, 4))),
                            Map.of());
            byte[] root =
                    ZLinkCanonicalUserSpotRelocationEnvelope.encode(envelope, id, 5, participants);
            var storeRequest =
                    new ZLinkAggregateRelocationCoordinator.Request(
                            id,
                            1,
                            1,
                            participants.stream()
                                    .map(
                                            participant ->
                                                    new ZLinkAggregateRelocationCoordinator
                                                            .Participant(
                                                            participant.authorityKey(),
                                                            participant.objectId().equals(ACTOR)
                                                                    ? ZLinkPlacementObjectKind.ACTOR
                                                                    : ZLinkPlacementObjectKind
                                                                            .USER_SPOT,
                                                            participant.objectGeneration(),
                                                            participant
                                                                    .sourceAuthorityOwnerGeneration(),
                                                            "version-1",
                                                            ZLinkAuthorityGenerationTransition
                                                                    .NEW_OWNER,
                                                            sourceAuthority(
                                                                    participant.objectId(), SOURCE),
                                                            new byte[0]))
                                    .toList(),
                            root,
                            new ZLinkMeshNodeDescriptorKey("mesh", TARGET),
                            17,
                            new ZLinkPlacementCapacityBundle(
                                    1,
                                    1,
                                    Optional.of(
                                            new ZLinkSpotTypeCapacityDelta(
                                                    ZLinkPlacementObjectKind.USER_SPOT,
                                                    "room",
                                                    1))),
                            new ZLinkLocationOwnerToken("target-owner", 23),
                            "version-1");
            authority.seed(storeRequest);
            var prepared =
                    coordinator.prepare(storeRequest, () -> false).toCompletableFuture().join();
            var backend =
                    new FakeStagingBackend() {
                        @Override
                        public java.util.concurrent.CompletionStage<Optional<byte[]>> replayActor(
                                Object spot,
                                Object actor,
                                ZLinkActorAcceptedJournal.Record record) {
                            byte[] payload = record.payload();
                            executed.add("actor-" + payload[0]);
                            return CompletableFuture.completedFuture(Optional.of(payload));
                        }

                        @Override
                        public java.util.concurrent.CompletionStage<List<byte[]>> replaySpot(
                                Object spot, ZLinkSpotAcceptedJournal.Record record) {
                            byte[] payload = record.parts().getLast();
                            executed.add("spot-" + payload[0]);
                            return CompletableFuture.completedFuture(List.of(payload));
                        }
                    };
            endpoint =
                    new ZLinkUserSpotRetireTargetEndpoint(
                            TARGET,
                            17,
                            coordinator,
                            new ZLinkUserSpotAggregateStagingOwner(backend),
                            ignored -> TestSpot.class,
                            (lane, record) -> CompletableFuture.completedFuture(null),
                            null,
                            Duration.ofSeconds(1),
                            ignored -> CompletableFuture.completedFuture(null),
                            null,
                            null,
                            store,
                            null);
            request =
                    new ZLinkSpotRetireControl.StageRequest(
                            new ZLinkSpotRetireControl.Fence(id, 1),
                            SOURCE,
                            11,
                            "source-owner",
                            12,
                            TARGET,
                            17,
                            "target-owner",
                            23,
                            "mesh",
                            SPOT,
                            "room",
                            false,
                            false,
                            root,
                            participants);
            endpoint.stage(request).toCompletableFuture().join();
            coordinator.commit(prepared, () -> false).toCompletableFuture().join();
            committed.set(true);
        }

        CompletableFuture<Integer> actor(PeerAuthorityFence source, int value) {
            var result = new CompletableFuture<Integer>();
            boolean accepted =
                    endpoint.handleActor(
                            source,
                            new ActorMessage(
                                    true,
                                    0,
                                    (long) value,
                                    null,
                                    new ActorRouteFence(
                                            new ZLinkBackendActorRef(TARGET, ACTOR, 4), 17, 8, 23)),
                            () ->
                                    ZLinkAcceptedJournalTestRecords.actor(
                                            ACTOR,
                                            value,
                                            "probe",
                                            Map.of(),
                                            new byte[] {(byte) value}),
                            List.of(),
                            null,
                            parts -> complete(result, parts),
                            result::completeExceptionally);
            if (!accepted)
                result.completeExceptionally(new AssertionError("Actor ingress rejected"));
            return result;
        }

        CompletableFuture<Integer> spot(long generation, int value) {
            var result = new CompletableFuture<Integer>();
            boolean accepted =
                    endpoint.handleSpot(
                            new PeerAuthorityFence(PEER, 21, "peer-owner", 31),
                            new SpotMessage(
                                    true,
                                    0,
                                    (long) value,
                                    "source-spot",
                                    new SpotRouteFence(SPOT, 3, TARGET, 17, generation, 23)),
                            new byte[0],
                            () -> acceptedSpotRequest(SPOT, value),
                            0,
                            List.of(),
                            null,
                            parts -> complete(result, parts),
                            result::completeExceptionally);
            if (!accepted)
                result.completeExceptionally(new AssertionError("Spot ingress rejected"));
            return result;
        }

        void open() throws Exception {
            endpoint.publish(request).toCompletableFuture().get(5, TimeUnit.SECONDS);
        }

        private static void complete(CompletableFuture<Integer> result, List<Message> parts) {
            try (var part = parts.getFirst()) {
                result.complete((int) part.dataBuffer().get(0));
            }
        }
    }
}
