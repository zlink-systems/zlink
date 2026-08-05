package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

final class ZLinkStandaloneActorRelocationStagingOwnerTest {
    @Test
    void actorStaysHiddenUntilReplayAndExplicitPublication() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        var request = request(relocationId, true);
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            7,
            11,
            true,
            new byte[] {4, 5},
            List.of());

        var staged = owner.stage(request, root)
            .toCompletableFuture().join();

        assertEquals(List.of("prepare"), backend.operations);
        assertFalse(backend.visible);

        owner.replayHidden(staged).toCompletableFuture().join();
        assertFalse(backend.visible);
        owner.publish(staged);
        assertTrue(backend.visible);
        assertFalse(backend.admitted);
        owner.openAdmission(staged);
        assertTrue(backend.admitted);
        assertEquals(
            List.of("prepare", "publish", "open"),
            backend.operations);
    }

    @Test
    void discardedTargetNeverBecomesVisible() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        var staged = owner.stage(
                request(relocationId, false),
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    "actor-a",
                    7,
                    11,
                    false,
                    new byte[0],
                    List.of()))
            .toCompletableFuture().join();

        owner.discard(staged).toCompletableFuture().join();

        assertFalse(backend.visible);
        assertTrue(backend.discarded);
        assertThrows(IllegalStateException.class, () -> owner.publish(staged));
    }

    @Test
    void authoritySelectedRootPublishesHiddenActorButKeepsAdmissionClosed() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            7,
            11,
            true,
            new byte[] {4, 5},
            List.of());
        var staged = owner.stage(request(relocationId, true), root)
            .toCompletableFuture().join();

        owner.publishAndReplayHidden(staged, root)
            .toCompletableFuture().join();

        assertTrue(backend.visible);
        assertFalse(backend.admitted);
        assertEquals(List.of("prepare", "publish"), backend.operations);
        owner.openAdmission(staged);
        assertEquals(List.of("prepare", "publish", "open"), backend.operations);
    }

    @Test
    void canonicalReplayerPreservesActorReplyCapabilityBeforePublication() {
        FakeBackend backend = new FakeBackend();
        backend.replayReply = Optional.of(new byte[] {9, 8});
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] accepted = ZLinkAcceptedJournalTestRecords.actor(
            "actor-a",
            23,
            "actor.request",
            java.util.Map.of("trace", "a"),
            new byte[] {1, 2});
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            7,
            11,
            true,
            new byte[] {4, 5},
            List.of(new ZLinkAsyncSerialQueue.QueuedRecord(5, accepted)));
        var staged = owner.stage(request(relocationId, true), root)
            .toCompletableFuture().join();
        AtomicReference<ZLinkActorAcceptedJournal.Record> relayed =
            new AtomicReference<>();

        var replayer = new ZLinkAcceptedJournalReplayer(
            record -> CompletableFuture.failedFuture(
                new AssertionError("standalone Actor must not replay Spot")),
            record -> owner.replayActor(staged, record),
            new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                @Override
                public CompletionStage<Void> completeSpot(
                    ZLinkSpotAcceptedJournal.Record record,
                    long acceptedSequence,
                    List<byte[]> reply) {
                    return CompletableFuture.failedFuture(
                        new AssertionError(
                            "standalone Actor must not relay Spot"));
                }

                @Override
                public CompletionStage<Void> completeActor(
                    ZLinkActorAcceptedJournal.Record record,
                    long acceptedSequence,
                    Optional<byte[]> reply) {
                    assertEquals(5, acceptedSequence);
                    assertEquals(23, record.replyRouteId().orElseThrow());
                    assertEquals("journal-owner", record.sourceOwnerId());
                    assertEquals(1, record.sourceOwnerLeaseGeneration());
                    assertEquals("journal-node", record.sourceNodeRid().toString());
                    assertEquals(1, record.sourceNodeGeneration());
                    assertArrayEquals(
                        new byte[] {9, 8}, reply.orElseThrow());
                    relayed.set(record);
                    return CompletableFuture.completedFuture(null);
                }
            });

        owner.publishAndReplayHidden(staged, root, replayer)
            .toCompletableFuture().join();

        assertNotNull(relayed.get());
        assertTrue(backend.visible);
        assertFalse(backend.admitted);
        assertEquals(
            List.of("prepare", "replay", "publish"),
            backend.operations);
    }

    @Test
    void authoritySelectedRootCannotReplaceCapturedApplicationState() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        var staged = owner.stage(
                request(relocationId, true),
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    "actor-a",
                    7,
                    11,
                    true,
                    new byte[] {4, 5},
                    List.of()))
            .toCompletableFuture().join();
        byte[] changed = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            7,
            11,
            true,
            new byte[] {9},
            List.of());

        assertThrows(
            IllegalArgumentException.class,
            () -> owner.publishAndReplayHidden(staged, changed));
        assertFalse(backend.visible);
    }

    @Test
    void rootMustMatchActorAuthorityFence() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            8,
            11,
            false,
            new byte[0],
            List.of());

        assertThrows(
            IllegalArgumentException.class,
            () -> owner.stage(request(relocationId, true), root));
        assertTrue(backend.operations.isEmpty());
    }

    private static ZLinkStandaloneActorRelocationStagingOwner.Request request(
        UUID relocationId,
        boolean restoreSnapshot) {
        return new ZLinkStandaloneActorRelocationStagingOwner.Request(
            relocationId,
            "actor-a",
            "player",
            7,
            11,
            restoreSnapshot,
            "target-entry");
    }

    private static final class FakeBackend
        implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final List<String> operations = new ArrayList<>();
        private boolean visible;
        private boolean admitted;
        private boolean discarded;
        private Optional<byte[]> replayReply;

        @Override
        public CompletionStage<Object> prepare(
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            byte[] state,
            ZLinkRelocationCancellation cancellation) {
            operations.add("prepare");
            assertArrayEquals(
                request.restoreSnapshot() ? new byte[] {4, 5} : new byte[0],
                state);
            assertFalse(cancellation.isCancellationRequested());
            return CompletableFuture.completedFuture("prepared");
        }

        @Override
        public CompletionStage<Optional<byte[]>> replay(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request,
            ZLinkActorAcceptedJournal.Record record) {
            if (replayReply == null) {
                fail("empty journal must not dispatch a record");
            }
            operations.add("replay");
            return CompletableFuture.completedFuture(replayReply);
        }

        @Override
        public void publish(
            Object actor,
            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            operations.add("publish");
            visible = true;
        }

        @Override
        public void openAdmission(Object actor) {
            operations.add("open");
            admitted = true;
        }

        @Override
        public CompletionStage<Void> discard(Object actor) {
            operations.add("discard");
            discarded = true;
            return CompletableFuture.completedFuture(null);
        }
    }
}
