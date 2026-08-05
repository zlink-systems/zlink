package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkUserSpotRelocationEnvelopeTest {
    @Test
    void canonicalEnvelopeRoundTripsParticipantsTimersAndJournal() {
        RoutingId source = RoutingId.from("source-node");
        RoutingId target = RoutingId.from("target-node");
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
            new LinkedHashMap<>();
        journal.put("spot", List.of(
            new ZLinkAsyncSerialQueue.QueuedRecord(3,
                ZLinkAcceptedJournalTestRecords.spot(
                    "source", "room-a", 0, "first", Map.of(),
                    new byte[] {3})),
            new ZLinkAsyncSerialQueue.QueuedRecord(4,
                ZLinkAcceptedJournalTestRecords.spot(
                    "source", "room-a", 0, "second", Map.of(),
                    new byte[] {4}))));
        journal.put("actor:actor-a", List.of(
            new ZLinkAsyncSerialQueue.QueuedRecord(7,
                ZLinkAcceptedJournalTestRecords.actor(
                    "actor-a", 0, "actor", Map.of(), new byte[] {7}))));
        byte[] timerEnvelope = ZLinkSpotTimerRelocationEnvelope.encodeCanonical(
            List.of(new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
                "heartbeat", TestSpot.class.getName(), 1000, 1, 1, true,
                5, 6, 1_700_000_000_000L, null)));
        byte[] actorTimerEnvelope =
            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(
                List.of(new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
                    "actor-heartbeat", TestSpot.class.getName(),
                    250, 2, 3, false, 8, 9, 1_700_000_000_250L,
                    new ZLinkSpotTimerRelocationEnvelope.CanonicalPending(
                        10, 11, 1_700_000_000_200L, 2))));
        var request = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            "room-a",
            5,
            new byte[] {1, 2},
            true,
            timerEnvelope,
            List.of(new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                "actor-a",
                "player",
                new byte[] {6},
                true,
                new ZLinkBackendActorRef(source, "actor-a", 11),
                actorTimerEnvelope)),
            journal);

        UUID relocationId = UUID.randomUUID();
        var participants = List.of(
            new ZLinkSpotRetireControl.ParticipantFence(
                "actor/actor-a", 1, "actor-a", "player", true, 11, 4),
            new ZLinkSpotRetireControl.ParticipantFence(
                "spot/room-a", 2, "room-a", "room", true, 5, 9));
        var stage = stage(relocationId, source, target, participants, true);
        var decoded = ZLinkCanonicalUserSpotRelocationEnvelope.decode(
            ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                request, relocationId, 9, participants),
            target,
            stableType -> stableType.equals("room") ? TestSpot.class : null,
            stage);

        assertEquals("room-a", decoded.spotId());
        assertEquals(5, decoded.objectGeneration());
        assertArrayEquals(new byte[] {1, 2}, decoded.spotState());
        assertEquals("heartbeat",
            ZLinkSpotTimerRelocationEnvelope.canonicalize(
                decoded.timerEnvelope()).getFirst().name());
        assertEquals(target, decoded.actors().getFirst()
            .preparedActorRef().nodeRid());
        assertEquals(11, decoded.actors().getFirst()
            .preparedActorRef().generation());
        var actorTimer = ZLinkSpotTimerRelocationEnvelope.canonicalize(
            decoded.actors().getFirst().timerEnvelope()).getFirst();
        assertEquals("actor-heartbeat", actorTimer.name());
        assertEquals(10, actorTimer.pending().deliveryIndex());
        assertEquals(7, decoded.acceptedJournal()
            .get("actor:actor-a").getFirst().sequence());
    }

    @Test
    void unknownStableTypeAndTrailingBytesAreRejected() {
        RoutingId node = RoutingId.from("node");
        var request = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            "room-a",
            1,
            new byte[0],
            false,
            new byte[0],
            List.of(),
            java.util.Map.of());
        UUID relocationId = UUID.randomUUID();
        var participants = List.of(
            new ZLinkSpotRetireControl.ParticipantFence(
                "spot/room-a", 2, "room-a", "room", false, 1, 1));
        var stage = stage(relocationId, node, node, participants, false);
        byte[] encoded = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
            request, relocationId, 1, participants);

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                encoded,
                node,
                ignored -> null,
                stage));
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalUserSpotRelocationEnvelope.decode(
                java.util.Arrays.copyOf(encoded, encoded.length + 1),
                node,
                ignored -> TestSpot.class,
                stage));
    }

    private static ZLinkSpotRetireControl.StageRequest stage(
        UUID relocationId,
        RoutingId source,
        RoutingId target,
        List<ZLinkSpotRetireControl.ParticipantFence> participants,
        boolean restoreSpotSnapshot) {
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            source, 1, "source-owner", 1,
            target, 2, "target-owner", 2,
            "mesh", "room-a", "room", false, restoreSpotSnapshot,
            "relocation/root", 0, participants);
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override public ZLinkSpotContext context() { return null; }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
