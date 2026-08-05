package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

final class ZLinkUserSpotAggregateStagingOwnerTest {
    @Test
    void noParticipantIsVisibleBeforeAggregatePublish() {
        FakeBackend backend = new FakeBackend();
        ZLinkUserSpotAggregateStagingOwner owner =
            new ZLinkUserSpotAggregateStagingOwner(backend);

        var staged = owner.stage(request(), () -> false)
            .toCompletableFuture().join();

        assertTrue(backend.live.isEmpty());
        assertEquals(
            List.of("prepare:spot", "restore:spot", "prepare:actor-a",
                "prepare:actor-b"),
            backend.operations);

        owner.publishAndReplay(staged, (lane, record) -> {
                assertTrue(backend.live.isEmpty());
                backend.operations.add("replay:" + lane + ":" + record.sequence());
                return CompletableFuture.completedFuture(null);
            })
            .toCompletableFuture().join();

        assertEquals(List.of("actor-a", "actor-b", "spot"), backend.live);
        assertEquals("timers:publish", backend.operations.getLast());
        assertEquals(
            1,
            backend.operations.stream()
                .filter("relocation-ready"::equals)
                .count());
        assertTrue(backend.operations.indexOf("relocation-ready")
            < backend.operations.indexOf("replay:spot:1"));
        assertTrue(backend.operations.indexOf("replay:spot:1")
            < backend.operations.indexOf("timers:publish"));
    }

    @Test
    void partialActorRestoreFailureDiscardsAllStagingWithoutPublication() {
        FakeBackend backend = new FakeBackend();
        backend.failActor = "actor-b";
        ZLinkUserSpotAggregateStagingOwner owner =
            new ZLinkUserSpotAggregateStagingOwner(backend);

        assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> owner.stage(request(), () -> false)
                .toCompletableFuture().join());

        assertTrue(backend.live.isEmpty());
        assertTrue(backend.operations.contains("discard:actor-a"));
        assertEquals("discard:spot", backend.operations.getLast());
    }

    @Test
    void finalRootMustPreserveTheInitialFactoryAndRestoreState() {
        FakeBackend backend = new FakeBackend();
        ZLinkUserSpotAggregateStagingOwner owner =
            new ZLinkUserSpotAggregateStagingOwner(backend);
        var staged = owner.stage(request(), () -> false)
            .toCompletableFuture().join();
        var changed = new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            "room-a",
            7,
            new byte[] {99},
            true,
            new byte[] {8},
            List.of(actor("actor-a"), actor("actor-b")),
            request().acceptedJournal());

        assertThrows(
            IllegalArgumentException.class,
            () -> owner.publishAndReplay(
                staged,
                changed,
                (lane, record) -> CompletableFuture.completedFuture(null)));
        assertTrue(backend.live.isEmpty());
    }

    @Test
    void actorTimersAreStagedBeforeAggregatePublication() {
        FakeBackend backend = new FakeBackend();
        ZLinkUserSpotAggregateStagingOwner owner =
            new ZLinkUserSpotAggregateStagingOwner(backend);
        byte[] timerEnvelope =
            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(
                List.of(new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
                    "actor-heartbeat", TestSpot.class.getName(),
                    1000, 1, 1, true, 2, 3, 4, null)));
        var base = request();
        var timed = new ZLinkUserSpotAggregateStagingOwner.Request(
            base.spotType(),
            base.spotStableType(),
            base.spotId(),
            base.objectGeneration(),
            base.spotState(),
            base.restoreSpotSnapshot(),
            base.timerEnvelope(),
            List.of(
                new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                    "actor-a", "player", new byte[] {4}, true, null,
                    timerEnvelope),
                actor("actor-b")),
            base.acceptedJournal());

        owner.stage(timed, () -> false).toCompletableFuture().join();

        assertTrue(backend.operations.indexOf("timers:stage:actor-a")
            > backend.operations.indexOf("prepare:actor-a"));
        assertTrue(backend.operations.indexOf("timers:stage:actor-a")
            < backend.operations.indexOf("prepare:actor-b"));
    }

    private static ZLinkUserSpotAggregateStagingOwner.Request request() {
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
            new LinkedHashMap<>();
        journal.put("spot", List.of(
            new ZLinkAsyncSerialQueue.QueuedRecord(1, new byte[] {1}),
            new ZLinkAsyncSerialQueue.QueuedRecord(2, new byte[] {2})));
        journal.put("actor:actor-a", List.of(
            new ZLinkAsyncSerialQueue.QueuedRecord(3, new byte[] {3})));
        return new ZLinkUserSpotAggregateStagingOwner.Request(
            TestSpot.class,
            "room",
            "room-a",
            7,
            new byte[] {9},
            true,
            new byte[] {8},
            List.of(actor("actor-a"), actor("actor-b")),
            journal);
    }

    private static ZLinkUserSpotAggregateStagingOwner.ActorParticipant actor(
        String id) {
        return new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
            id,
            "player",
            new byte[] {4},
            true,
            null);
    }

    private static final class FakeBackend
        implements ZLinkUserSpotAggregateStagingOwner.StagingBackend {
        private final List<String> operations = new ArrayList<>();
        private final List<String> live = new ArrayList<>();
        private String failActor;

        @Override
        public CompletionStage<Object> prepareSpot(
            ZLinkUserSpotAggregateStagingOwner.Request request) {
            operations.add("prepare:spot");
            return CompletableFuture.completedFuture("spot");
        }

        @Override
        public CompletionStage<Void> restoreSpot(
            Object preparedSpot,
            ZLinkUserSpotAggregateStagingOwner.Request request,
            ZLinkRelocationCancellation cancellation) {
            operations.add("restore:spot");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Object> prepareActor(
            ZLinkUserSpotAggregateStagingOwner.ActorParticipant participant,
            ZLinkRelocationCancellation cancellation) {
            operations.add("prepare:" + participant.actorId());
            return participant.actorId().equals(failActor)
                ? CompletableFuture.failedFuture(
                    new IllegalStateException("Restore failed"))
                : CompletableFuture.completedFuture(participant.actorId());
        }

        @Override
        public CompletionStage<Void> completeRelocationReady(
            Object preparedSpot) {
            operations.add("relocation-ready");
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> stageActorTimers(
            Object preparedActor,
            byte[] timerEnvelope) {
            if (!ZLinkSpotTimerRelocationEnvelope
                    .canonicalize(timerEnvelope).isEmpty()) {
                operations.add("timers:stage:" + preparedActor);
            }
            return CompletableFuture.completedFuture(null);
        }

        @Override public void publishSpot(Object value) {
            live.add((String) value);
            operations.add("publish:" + value);
        }

        @Override public void publishActor(Object value) {
            live.add((String) value);
            operations.add("publish:" + value);
        }

        @Override public void completeActor(Object value) {
            operations.add("complete:" + value);
        }

        @Override public void publishTimers(Object value) {
            operations.add("timers:publish");
        }

        @Override
        public CompletionStage<Void> discardActor(Object value) {
            operations.add("discard:" + value);
            return CompletableFuture.completedFuture(null);
        }

        @Override public void discardSpot(Object value) {
            operations.add("discard:spot");
        }
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
