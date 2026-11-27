package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.acceptedSpotRequest;
import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.recoveryNode;
import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.sourceAuthority;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationDeleteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationRenewResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkSpotTypeCapacityDelta;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.AuthorityState;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.FakeStagingBackend;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.TestSpot;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkTargetRelocationCompletionsTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @ParameterizedTest
    @ValueSource(ints = {1, 2})
    void persistsRepliesBeforeRelayAndSettlesAfterAcknowledgement(int requests) {
        //  The frozen record's request-source fence names "journal-node"
        //  while the relocation source (the node that owns the landed reply
        //  capability) is "source-node". Command 33 must be addressed to the
        //  relocation source, not to the request-source fence.
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId journalRid = RoutingId.from("journal-node");
        RoutingId targetRid = RoutingId.from("target-node");
        long targetNodeGeneration = 17;
        long targetAttemptGeneration = 41;
        String spotId = "room-a";
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var trace = authority.completionTrace;
        var memory = new InMemoryRelocationStore();
        ZLinkRelocationStore blobs =
                new ZLinkRelocationStore() {
                    @Override
                    public CompletionStage<ZLinkRelocationStored> put(
                            byte[] payload,
                            Duration retention,
                            ZLinkStoreCancellation cancellation) {
                        trace.add("put");
                        return memory.put(payload, retention, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkRelocationReadResult> get(
                            String reference, ZLinkStoreCancellation cancellation) {
                        trace.add("get");
                        return memory.get(reference, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkRelocationRenewResult> renew(
                            String reference,
                            Duration retention,
                            ZLinkStoreCancellation cancellation) {
                        trace.add("renew");
                        return memory.renew(reference, retention, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkRelocationDeleteResult> delete(
                            String reference, ZLinkStoreCancellation cancellation) {
                        assertNull(authority.rows.get(authorityKey).relocation());
                        trace.add("delete");
                        return memory.delete(reference, cancellation);
                    }
                };
        var coordinator = new ZLinkAggregateRelocationCoordinator(authorityStore, ignored -> true);
        UUID aggregateId = UUID.randomUUID();
        var participants =
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                actorAuthorityKey, 1, actorId, "player", false, 4, 7),
                        new ZLinkSpotRetireControl.ParticipantFence(
                                authorityKey, 2, spotId, "room", true, 3, 5));
        var finalEnvelope =
                new ZLinkUserSpotAggregateStagingOwner.Request(
                        TestSpot.class,
                        "room",
                        spotId,
                        3,
                        new byte[] {1},
                        true,
                        new byte[0],
                        List.of(
                                new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                        actorId,
                                        "player",
                                        new byte[0],
                                        false,
                                        new ZLinkBackendActorRef(targetRid, actorId, 4))),
                        Map.of(
                                "spot",
                                List.of(
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1, acceptedSpotRequest(spotId, 2)))));
        var storeParticipants =
                List.of(
                        new ZLinkAggregateRelocationCoordinator.Participant(
                                actorAuthorityKey,
                                ZLinkPlacementObjectKind.ACTOR,
                                4,
                                7,
                                "actor-version-1",
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                sourceAuthority(actorId, sourceRid),
                                new byte[0]),
                        new ZLinkAggregateRelocationCoordinator.Participant(
                                authorityKey,
                                ZLinkPlacementObjectKind.USER_SPOT,
                                3,
                                5,
                                "version-1",
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                sourceAuthority(spotId, sourceRid),
                                new byte[0]));
        var storeRequest =
                new ZLinkAggregateRelocationCoordinator.Request(
                        aggregateId,
                        targetAttemptGeneration,
                        targetAttemptGeneration,
                        storeParticipants,
                        ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                                finalEnvelope, aggregateId, 5, participants),
                        new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                        targetNodeGeneration,
                        new ZLinkPlacementCapacityBundle(
                                1,
                                1,
                                Optional.of(
                                        new ZLinkSpotTypeCapacityDelta(
                                                ZLinkPlacementObjectKind.USER_SPOT, "room", 1))),
                        new ZLinkLocationOwnerToken("target-owner", 23),
                        "version-1");
        authority.seed(storeRequest);
        var finalPrepared = coordinator.prepare(storeRequest, OPEN).toCompletableFuture().join();
        AtomicInteger relays = new AtomicInteger();
        List<RoutingId> relayDestinations = new CopyOnWriteArrayList<>();
        List<
                        systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.RequestSourceFence>
                relayExpectedSources = new CopyOnWriteArrayList<>();
        List<Long> relayAttempts = new CopyOnWriteArrayList<>();
        FakeStagingBackend backend = new FakeStagingBackend();
        var staging = new ZLinkUserSpotAggregateStagingOwner(backend);
        var wire = new ZLinkServiceM6BWireCodec();
        ZLinkInternalMeshNode peer =
                recoveryNode(
                        wire,
                        backend.operations,
                        relays,
                        relayDestinations,
                        relayExpectedSources,
                        relayAttempts);
        var endpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
                        blobs,
                        staging,
                        ignored -> TestSpot.class,
                        (lane, record) ->
                                CompletableFuture.failedFuture(
                                        new AssertionError(
                                                "canonical recovery must use the production"
                                                        + " replayer")),
                        new ZLinkSessionRelocationPeerClient(peer),
                        Duration.ofSeconds(1),
                        stage ->
                                coordinator.normalizePublishedAggregate(
                                        participants.stream()
                                                .map(
                                                        participant ->
                                                                new ZLinkAggregateRelocationCoordinator
                                                                        .ExpectedParticipant(
                                                                        participant.authorityKey(),
                                                                        participant
                                                                                .objectGeneration(),
                                                                        participant
                                                                                .sourceAuthorityOwnerGeneration()))
                                                .toList(),
                                        new ZLinkAggregateFence(
                                                aggregateId, targetAttemptGeneration),
                                        storeRequest.targetOwner(),
                                        OPEN),
                        null,
                        ZLinkSpotRetireControl.client(peer),
                        authorityStore,
                        null);
        var request =
                new ZLinkSpotRetireControl.StageRequest(
                        new ZLinkSpotRetireControl.Fence(aggregateId, targetAttemptGeneration),
                        sourceRid,
                        11,
                        "source-owner",
                        12,
                        targetRid,
                        targetNodeGeneration,
                        "target-owner",
                        23,
                        "mesh",
                        spotId,
                        "room",
                        false,
                        true,
                        storeRequest.root(),
                        participants);

        endpoint.stage(request).toCompletableFuture().join();
        var firstPutEntered = new CompletableFuture<Void>();
        var releaseFirstPut = new CompletableFuture<Void>();
        var resubmissions = new AtomicInteger();
        List<byte[]> verified = List.of();
        if (requests == 2) {
            byte[] concurrentRequest =
                    ZLinkAcceptedJournalTestRecords.spot(
                            "source", spotId, 43, "packet-3", Map.of(), new byte[] {3}, 3);
            endpoint.stageRelayedRecord(request, concurrentRequest).toCompletableFuture().join();
            verified = List.of(concurrentRequest);
            memory.gateNextInternalPut(firstPutEntered, releaseFirstPut);
        }
        var completion =
                endpoint.prepareCompletions(
                        request,
                        verified,
                        operation -> {
                            resubmissions.incrementAndGet();
                            operation.run();
                        });
        coordinator
                .commit(finalPrepared, completion.acceptedRequests(), OPEN)
                .toCompletableFuture()
                .join();
        assertEquals(
                Integer.toString(requests),
                authority
                        .rows
                        .get(authorityKey)
                        .relocation()
                        .json()
                        .path("completion")
                        .path("acceptedRequests")
                        .asText(),
                "target commit must include the source-accepted request count");

        var premature =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () ->
                                coordinator
                                        .advancePublished(
                                                List.of(
                                                        new ZLinkAggregateRelocationCoordinator
                                                                .ExpectedParticipant(
                                                                authorityKey, 3, 5)),
                                                new ZLinkAggregateFence(
                                                        aggregateId, targetAttemptGeneration),
                                                "completed",
                                                OPEN)
                                        .toCompletableFuture()
                                        .join());
        assertInstanceOf(
                ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                premature.getCause());
        authority.dropCompletionCasResponse = true;
        try {
            endpoint.publish(request, completion).toCompletableFuture().join();
            if (requests == 2) {
                assertTrue(firstPutEntered.isDone());
                assertEquals(
                        1, relays.get(), "another request can finish while the first write waits");
                assertEquals(0, resubmissions.get());
            }
        } finally {
            releaseFirstPut.complete(null);
        }

        assertEquals(
                requests - 1, resubmissions.get(), "only the version conflict needs resubmission");
        assertEquals(requests, trace.stream().filter("pending-cas"::equals).count());
        assertEquals(requests, trace.stream().filter("ack-cas"::equals).count());
        if (requests == 1) {
            assertTrue(trace.indexOf("pending-cas") >= 4, trace::toString);
            assertEquals(List.of("put", "get", "put", "get"), trace.subList(0, 4));
        }
        assertTrue(trace.indexOf("confirmed-cas") > trace.indexOf("pending-cas"), trace::toString);
        assertTrue(trace.indexOf("ack-cas") > trace.indexOf("confirmed-cas"), trace::toString);
        assertTrue(trace.indexOf("completed") > trace.lastIndexOf("ack-cas"), trace::toString);
        assertTrue(trace.indexOf("delete") > trace.indexOf("completed"), trace::toString);
        assertNull(authority.rows.get(authorityKey).relocation());

        assertEquals(requests, relays.get(), "each preserved request reply must relay once");
        assertEquals(
                java.util.Collections.nCopies(requests, sourceRid),
                relayDestinations,
                "command 33 must be addressed to the relocation source node");
        assertEquals(requests, relayExpectedSources.size());
        relayExpectedSources.forEach(
                expected ->
                        assertEquals(
                                journalRid,
                                expected.nodeRid(),
                                "the expected source keeps the request-source lease fence"));
        assertEquals(
                java.util.Collections.nCopies(requests, targetAttemptGeneration),
                relayAttempts,
                "command 33 must echo the PREPARE targetAttemptGeneration");
    }
}
