package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.acceptedSpotRequest;
import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.recoveryNode;
import static systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.sourceAuthority;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationReplyRow;
import systems.zlink.framework.runtime.internal.locations.ZLinkSpotTypeCapacityDelta;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.CoordinatorFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.Operation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.RelocationId;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.ReplyRelay;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec.RequestSourceFence;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec.ApplicationPayloadEnvelopeV1;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.AuthorityState;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.FakeStagingBackend;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireTargetEndpointTest.TestSpot;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkTargetRelocationRepliesTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void earlierRequesterLeaseKeepsAlreadyTerminalReplyRowPending() {
        var timeout = Duration.ofSeconds(1);
        var factory = new ZLinkJavaBackendAdapterFactory();
        var options = new ZLinkBackendAdapterOptions(timeout);
        try (var context = factory.createChannelAdapter(options).createContext();
                var node = factory.createMeshAdapter(options).createMeshNode(context, "mesh")) {
            var rid = RoutingId.from("journal-node");
            var writer = new ZLinkLocationOwnerToken("journal-owner", 2);
            node.setRoutingId(rid);
            node.setBind("tcp://127.0.0.1:0");
            node.start();
            long generation = node.lifecycleGeneration();
            node.confirmLocalOwnerFence(
                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                            rid, generation, writer.ownerId(), writer.leaseGeneration()));

            var authority = new AuthorityState();
            var store = authority.proxy();
            var coordinator = new ZLinkAggregateRelocationCoordinator(store);
            var aggregateId = new UUID(4, 5);
            var objectKey = ZLinkAuthorityKeyCodec.spot("room-a");
            var participant =
                    new ZLinkSpotRetireControl.ParticipantFence(
                            objectKey, 2, "room-a", "room", true, 3, 5);
            var stage =
                    new ZLinkSpotRetireControl.StageRequest(
                            new ZLinkSpotRetireControl.Fence(aggregateId, 6),
                            rid,
                            generation,
                            writer.ownerId(),
                            1,
                            rid,
                            generation,
                            writer.ownerId(),
                            writer.leaseGeneration(),
                            "mesh",
                            "room-a",
                            "room",
                            false,
                            true,
                            new byte[] {1},
                            List.of(participant));

            var codec = new ZLinkServiceRelocationWireCodec();
            var relay =
                    new ReplyRelay(
                            new Operation(7, 8),
                            33,
                            new RelocationId(4, 5),
                            6,
                            new CoordinatorFence(
                                    writer.ownerId(),
                                    writer.leaseGeneration(),
                                    rid,
                                    generation,
                                    "initial"),
                            1,
                            1,
                            0,
                            0);
            var payload =
                    new ApplicationPayloadEnvelopeV1(
                            "reply", "application/octet-stream", new byte[] {42});
            var frames = codec.encodeReplyRelayFrames(relay, payload);
            var row =
                    new ZLinkRelocationReplyRow(
                            aggregateId,
                            objectKey,
                            new ZLinkRelocationReplyRow.Source(
                                    writer.ownerId(),
                                    "1",
                                    rid.toHex(),
                                    Long.toUnsignedString(generation)),
                            "00000000000000070000000000000008",
                            "33",
                            writer,
                            ZLinkRelocationReplyRow.PENDING,
                            codec.encodeStoredReply(relay, payload));
            authority.replyRows.put(row.key(), row.accepted());
            var replies =
                    new ZLinkTargetRelocationReplies(
                            stage, coordinator, store, Runnable::run, List.of(row.accepted()));
            var client = ZLinkSpotRetireControl.client(node);

            var current =
                    new RequestSourceFence(
                            writer.ownerId(), writer.leaseGeneration(), rid, generation);
            var ack =
                    codec.decodeReplyRelayAck(
                            client.relayCanonicalReply(
                                            rid,
                                            current,
                                            frames.getFirst(),
                                            frames.subList(1, frames.size()),
                                            timeout)
                                    .toCompletableFuture()
                                    .join());
            assertEquals(2, ack.status());
            assertEquals(current, ack.requestSource());

            var earlier = new RequestSourceFence(writer.ownerId(), 1, rid, generation);
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    replies.complete(
                                                    row,
                                                    () -> row,
                                                    ignored ->
                                                            client.relayCanonicalReply(
                                                                            rid,
                                                                            earlier,
                                                                            frames.getFirst(),
                                                                            frames.subList(
                                                                                    1,
                                                                                    frames.size()),
                                                                            timeout)
                                                                    .thenApply(
                                                                            encoded ->
                                                                                    ZLinkRelocationReplyRow
                                                                                            .ALREADY_TERMINAL))
                                            .toCompletableFuture()
                                            .join());
            assertInstanceOf(IllegalArgumentException.class, failure.getCause());
            assertEquals(
                    ZLinkRelocationReplyRow.PENDING, authority.replyRows.get(row.key()).state());
            assertEquals(List.of("pending-cas"), authority.completionTrace);
        }
    }

    @ParameterizedTest
    @CsvSource({"1, false, true", "2, false, true", "1, true, true", "1, false, false"})
    void persistsRepliesAndSettlesFromAcknowledgementOrExpiredLease(
            int requests, boolean sourceExpired, boolean acknowledge) {
        // The original requester and the relocation source use different routing IDs.
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
        authority.sourceExpired = sourceExpired;
        ZLinkLocationRepository authorityStore = authority.proxy();
        var trace = authority.completionTrace;
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
                                new byte[] {'[', ']'}),
                        new ZLinkAggregateRelocationCoordinator.Participant(
                                authorityKey,
                                ZLinkPlacementObjectKind.USER_SPOT,
                                3,
                                5,
                                "version-1",
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                sourceAuthority(spotId, sourceRid),
                                new byte[] {'[', ']'}));
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
        if (!acknowledge) {
            ZLinkInternalMeshNode delegate = peer;
            peer =
                    (ZLinkInternalMeshNode)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    ZLinkInternalMeshNode.class.getClassLoader(),
                                    new Class<?>[] {ZLinkInternalMeshNode.class},
                                    (proxy, method, args) -> {
                                        Object result = method.invoke(delegate, args);
                                        return method.getName()
                                                        .equals("requestRelocationReplyRelay")
                                                ? CompletableFuture.failedFuture(
                                                        new java.util.concurrent.TimeoutException(
                                                                "command 46 ACK was not received"))
                                                : result;
                                    });
        }
        var endpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
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
            authority.pendingWriteEntered = firstPutEntered;
            authority.pendingWriteRelease = releaseFirstPut;
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
                .commit(finalPrepared, completion.acceptedRows(), OPEN)
                .toCompletableFuture()
                .join();
        assertEquals(requests, authority.replyRows.size());
        assertTrue(
                authority.replyRows.values().stream()
                        .allMatch(row -> row.state().equals("accepted")));
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

        assertEquals(0, resubmissions.get(), "independent rows do not conflict");
        assertEquals(requests, trace.stream().filter("pending-cas"::equals).count());
        assertEquals(acknowledge ? requests : 0, trace.stream().filter("ack-cas"::equals).count());
        if (requests == 2) {
            assertTrue(trace.indexOf("completed") < trace.lastIndexOf("ack-cas"), trace::toString);
        }
        assertNull(authority.rows.get(authorityKey).relocation());

        var states =
                authority.replyRows.values().stream()
                        .map(ZLinkRelocationReplyRow::state)
                        .sorted()
                        .toList();
        var expectedStates =
                !acknowledge
                        ? List.of(ZLinkRelocationReplyRow.PENDING)
                        : sourceExpired
                                ? List.of(ZLinkRelocationReplyRow.SOURCE_LEASE_EXPIRED)
                                : requests == 1
                                        ? List.of(ZLinkRelocationReplyRow.TERMINAL_RECEIVED)
                                        : List.of(
                                                ZLinkRelocationReplyRow.ALREADY_TERMINAL,
                                                ZLinkRelocationReplyRow.TERMINAL_RECEIVED);
        assertEquals(expectedStates, states);
        int expectedRelays = sourceExpired ? 0 : requests;
        assertEquals(expectedRelays, relays.get());
        assertEquals(
                java.util.Collections.nCopies(expectedRelays, journalRid),
                relayDestinations,
                "command 33 must reach the original requester");
        assertEquals(expectedRelays, relayExpectedSources.size());
        relayExpectedSources.forEach(
                expected ->
                        assertEquals(
                                journalRid,
                                expected.nodeRid(),
                                "the expected source keeps the request-source lease fence"));
        assertEquals(
                java.util.Collections.nCopies(expectedRelays, targetAttemptGeneration),
                relayAttempts,
                "command 33 must echo the PREPARE targetAttemptGeneration");
    }
}
