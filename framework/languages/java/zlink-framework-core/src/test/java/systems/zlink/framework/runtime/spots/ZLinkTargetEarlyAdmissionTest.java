package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
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
            opening.get(5, TimeUnit.SECONDS);
            assertEquals(List.of("spot-1"), fixture.executed);
            assertFalse(first.isDone());
            assertFalse(second.isDone());
            fixture.releaseRead.complete(null);
            fixture.backend.drainIngress();
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
            opening.get(5, TimeUnit.SECONDS);
            assertEquals(List.of("actor-1"), fixture.executed);
            assertFalse(spot.isDone());
            fixture.releaseRead.complete(null);
            fixture.backend.drainIngress();
            assertEquals(1, spot.get(5, TimeUnit.SECONDS));
            assertEquals(1, actor.get(5, TimeUnit.SECONDS));
        } finally {
            fixture.releaseRead.complete(null);
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void relayedAndConfirmedWorkReplayWhileLaterIngressAwaitsConfirmation(
            boolean readAfterNormalization) throws Exception {
        var fixture = new Fixture(true, readAfterNormalization);
        fixture.endpoint
                .stageRelayedRecord(fixture.request, acceptedSpotRequest(SPOT, 0))
                .toCompletableFuture()
                .join();
        var first = fixture.spot(6, 1);
        var opening = fixture.endpoint.publish(fixture.request).toCompletableFuture();
        assertFalse(opening.isDone());
        var late = new ArrayList<CompletableFuture<Integer>>();
        try {
            for (int value = 2; value <= 5; value++) {
                if (value != 5) fixture.blockedKey.set(SPOT_KEY);
                late.add(fixture.spot(6, value));
            }
            fixture.ready.complete(null);
            assertEquals(List.of("relayed-spot", "spot-1"), fixture.executed);
            assertEquals(1, first.join());
            assertTrue(late.stream().noneMatch(CompletableFuture::isDone));
            assertNull(
                    assertInstanceOf(
                                    ZLinkAuthoritySnapshot.class,
                                    fixture.authority
                                            .proxy()
                                            .read(SPOT_KEY, () -> false)
                                            .toCompletableFuture()
                                            .join())
                            .relocation());

            fixture.releaseRead.complete(null);
            fixture.backend.drainIngress();
            opening.get(5, TimeUnit.SECONDS);
            for (int index = 0; index < late.size(); index++) {
                assertEquals(index + 2, late.get(index).get(5, TimeUnit.SECONDS));
            }
            assertEquals(
                    List.of("relayed-spot", "spot-1", "spot-2", "spot-3", "spot-4", "spot-5"),
                    fixture.executed);
        } finally {
            fixture.releaseRead.complete(null);
            fixture.backend.drainIngress();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void confirmationReadsAuthorityAndUsesLocalLease(boolean leaseValid) throws Exception {
        var fixture = new Fixture();
        fixture.ownerLeaseValid.set(leaseValid);
        var before = fixture.authority.storeReads();
        var reply = fixture.spot(6, 1);
        var after = fixture.authority.storeReads();
        assertEquals(
                List.of(1, 0, 0),
                List.of(
                        after.get(0) - before.get(0),
                        after.get(1) - before.get(1),
                        after.get(2) - before.get(2)),
                "authority, aggregate progress, owner lease Store reads");
        if (leaseValid) {
            fixture.open();
            assertEquals(1, reply.get(5, TimeUnit.SECONDS));
        } else {
            var rejection =
                    assertThrows(java.util.concurrent.CompletionException.class, reply::join);
            assertArrayEquals(
                    new int[] {
                        RequestResult.CONFLICT.value(),
                        (int) ServiceWireConstants.FRAMEWORK_ERROR_SPOT_GENERATION_STALE
                    },
                    ZLinkRequestFailureMapping.outgoing(rejection));
            fixture.open();
            assertTrue(fixture.executed.isEmpty());
        }
    }

    private static final class Fixture {
        final AuthorityState authority = new AuthorityState();
        final List<String> executed = new ArrayList<>();
        final AtomicReference<String> blockedKey = new AtomicReference<>();
        final CompletableFuture<Void> readStarted = new CompletableFuture<>();
        final CompletableFuture<Void> releaseRead = new CompletableFuture<>();
        final AtomicBoolean committed = new AtomicBoolean();
        final AtomicBoolean ownerLeaseValid = new AtomicBoolean(true);
        final CompletableFuture<Void> spotRead = new CompletableFuture<>();
        final CompletableFuture<Void> actorRead = new CompletableFuture<>();
        final ZLinkUserSpotRetireTargetEndpoint endpoint;
        final ZLinkSpotRetireControl.StageRequest request;

        final CompletableFuture<Void> ready = new CompletableFuture<>();
        final FakeStagingBackend backend;

        Fixture() {
            this(false, false);
        }

        Fixture(boolean delayReady, boolean readAfterNormalization) {
            if (!delayReady) ready.complete(null);
            var store =
                    authority.proxy(
                            (key, value) -> {
                                if (key.equals(blockedKey.get())) {
                                    blockedKey.set(null);
                                    readStarted.complete(null);
                                    return releaseRead.thenCompose(
                                            ignored ->
                                                    readAfterNormalization
                                                            ? authority
                                                                    .proxy()
                                                                    .read(key, () -> false)
                                                            : CompletableFuture.completedFuture(
                                                                    value));
                                }
                                if (key.equals(SPOT_KEY) && committed.get())
                                    spotRead.complete(null);
                                if (key.equals(ACTOR_KEY) && committed.get()) {
                                    actorRead.complete(null);
                                }
                                return CompletableFuture.completedFuture(value);
                            });
            var coordinator =
                    new ZLinkAggregateRelocationCoordinator(
                            store,
                            owner ->
                                    ownerLeaseValid.get()
                                            && owner.equals(
                                                    new ZLinkLocationOwnerToken(
                                                            "target-owner", 23)));
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
                                                            new byte[] {'[', ']'}))
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
            backend =
                    new FakeStagingBackend(ZLinkExecutionLanePolicy.spot()) {
                        @Override
                        public java.util.concurrent.CompletionStage<Void> completeRelocationReady(
                                Object spot) {
                            return ready;
                        }

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
                            (lane, record) -> {
                                executed.add("relayed-" + lane);
                                return CompletableFuture.completedFuture(null);
                            },
                            null,
                            Duration.ofSeconds(1),
                            stage ->
                                    coordinator.normalizePublishedAggregate(
                                            stage.participants().stream()
                                                    .map(
                                                            participant ->
                                                                    new ZLinkAggregateRelocationCoordinator
                                                                            .ExpectedParticipant(
                                                                            participant
                                                                                    .authorityKey(),
                                                                            participant
                                                                                    .objectGeneration(),
                                                                            participant
                                                                                    .sourceAuthorityOwnerGeneration()))
                                                    .toList(),
                                            new ZLinkAggregateFence(
                                                    stage.fence().aggregateId(),
                                                    stage.fence().aggregateGeneration()),
                                            new ZLinkLocationOwnerToken(
                                                    stage.targetOwnerId(),
                                                    stage.targetOwnerLeaseGeneration()),
                                            () -> false),
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
            var publication =
                    new CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>();
            endpoint.stage(
                            request,
                            new ZLinkAggregateRelocationCoordinator.TargetIngressAuthority(
                                    () -> prepared, () -> publication.getNow(null)))
                    .toCompletableFuture()
                    .join();
            publication.complete(
                    coordinator.commit(prepared, () -> false).toCompletableFuture().join());
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
