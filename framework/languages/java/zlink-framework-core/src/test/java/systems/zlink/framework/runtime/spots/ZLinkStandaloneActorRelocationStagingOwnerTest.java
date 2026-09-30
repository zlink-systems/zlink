package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.drain.AsyncDrainProbe;
import systems.zlink.framework.runtime.mesh.ZLinkActivationAdmission;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkStandaloneActorRelocationStagingOwnerTest {
    @Test
    void successfulDiscardCompletesHeldIngressObligations() {
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(new FakeBackend());
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                        null,
                        ignored -> {}));
        owner.stageRelayedRecord(staged, new byte[] {2}).toCompletableFuture().join();
        assertEquals(2, pending(staged).size());

        owner.discard(staged).toCompletableFuture().join();
        assertTrue(pending(staged).isEmpty());
    }

    @Test
    void failedDiscardPreservesFailureAndNamedIngress() {
        FakeBackend backend = new FakeBackend();
        var failure = new IllegalStateException("discard failed");
        backend.discardResult = CompletableFuture.failedFuture(failure);
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                        null,
                        ignored -> {}));
        owner.stageRelayedRecord(staged, new byte[] {2}).toCompletableFuture().join();
        assertEquals(
                List.of(
                        new AsyncDrainProbe.Pending(
                                "temporary:actor:actor-a:0", "standalone Actor staging owner"),
                        new AsyncDrainProbe.Pending(
                                "relayed:actor:actor-a:0", "standalone Actor staging owner")),
                pending(staged));

        assertSame(
                failure,
                assertThrows(
                                java.util.concurrent.CompletionException.class,
                                () -> owner.discard(staged).toCompletableFuture().join())
                        .getCause());
        assertEquals(2, pending(staged).size());
    }

    private static List<AsyncDrainProbe.Pending> pending(Object owner) {
        try {
            var field = owner.getClass().getDeclaredField("debugProbe");
            field.setAccessible(true);
            return ((AsyncDrainProbe) field.get(owner)).pending();
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    @Test
    void failedBacklogReplayDoesNotWithholdLaterAcceptedRecord() {
        FakeBackend backend = new FakeBackend();
        backend.replayReply = Optional.empty();
        backend.replayFailuresRemaining = 1;
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        AtomicReference<Throwable> firstFailure = new AtomicReference<>();
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                        null,
                        firstFailure::set));
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 2, "actor.request", Map.of(), new byte[] {2}),
                        null,
                        ignored -> fail("later replay must succeed")));
        var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
        owner.publishHidden(backlog, 0);
        owner.openAdmission(staged);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () -> owner.drainDurableBacklog(backlog).toCompletableFuture().join());
        assertEquals(2, backend.operations.stream().filter("replay"::equals).count());
        assertInstanceOf(IllegalStateException.class, firstFailure.get());
    }

    @Test
    void failedDirectJoinReplayDoesNotWithholdLaterAcceptedRecord() {
        FakeBackend backend = new FakeBackend();
        backend.replayReply = Optional.empty();
        backend.replayFailuresRemaining = 1;
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        AtomicReference<Throwable> firstFailure = new AtomicReference<>();
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                        null,
                        firstFailure::set));
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 2, "actor.request", Map.of(), new byte[] {2}),
                        null,
                        ignored -> fail("later replay must succeed")));
        var replay = owner.closeDirectJoinIngress(staged, root);
        owner.publishDirectJoinHidden(replay, 12);
        owner.openAdmission(staged);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () ->
                        owner.replayDirectJoin(replay, actorReplayer(owner, staged))
                                .toCompletableFuture()
                                .join());
        assertEquals(2, backend.operations.stream().filter("replay"::equals).count());
        assertInstanceOf(IllegalStateException.class, firstFailure.get());
    }

    @Test
    void rejectedBacklogAdmissionFailsItsIngressAndStillSubmitsTheNext() {
        FakeBackend backend = new FakeBackend();
        backend.replayReply = Optional.empty();
        backend.rejectAdmissionsRemaining = 1;
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        AtomicReference<Throwable> rejected = new AtomicReference<>();
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                        null,
                        rejected::set));
        assertTrue(
                owner.acceptIngress(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 2, "actor.request", Map.of(), new byte[] {2}),
                        null,
                        ignored -> fail("later replay must succeed")));
        var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
        owner.publishHidden(backlog, 0);
        owner.openAdmission(staged);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () -> owner.drainDurableBacklog(backlog).toCompletableFuture().join());
        assertInstanceOf(IllegalStateException.class, rejected.get());
        assertEquals(1, backend.operations.stream().filter("replay"::equals).count());
    }

    @Test
    void actorStaysHiddenUntilDurableBacklogIsSealedAndPublished() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        var request = request(relocationId, true);
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());

        var staged = owner.stage(request, root).toCompletableFuture().join();

        assertEquals(List.of("prepare"), backend.operations);
        assertFalse(backend.visible);

        var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
        assertFalse(backend.visible);
        owner.publishHidden(backlog, 0);
        assertTrue(backend.visible);
        assertFalse(backend.admitted);
        owner.openAdmission(staged);
        assertTrue(backend.admitted);
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        assertEquals(List.of("prepare", "publish", "open"), backend.operations);
    }

    @Test
    void relocationTargetRestoreHoldsOneActivationAdmissionUntilCommitOrDiscard() {
        var admission = new ZLinkActivationAdmission(1);
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(new FakeBackend(), admission);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());

        // MeshNode §5.1: the Restore holds one admission from reception to target commit.
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        assertEquals(1, admission.snapshot().active());
        var full =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () ->
                                owner.stage(request(relocationId, true), root)
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(ZLinkFrameworkException.class, full.getCause()).kind());
        owner.publishHidden(
                owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged)), 0);
        assertEquals(0, admission.snapshot().active());

        var discarded = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        assertEquals(1, admission.snapshot().active());
        owner.discard(discarded).toCompletableFuture().join();
        assertEquals(0, admission.snapshot().active());
    }

    @Test
    void discardedTargetNeverBecomesVisible() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, false, new byte[0], List.of());
        var staged = owner.stage(request(relocationId, false), root).toCompletableFuture().join();
        AtomicReference<Throwable> stagedFailure = new AtomicReference<>();
        assertTrue(owner.acceptIngress(staged, new byte[] {1}, null, stagedFailure::set));

        owner.discard(staged).toCompletableFuture().join();

        assertFalse(backend.visible);
        assertTrue(backend.discarded);
        assertInstanceOf(IllegalStateException.class, stagedFailure.get());
        assertThrows(
                IllegalStateException.class,
                () -> owner.acceptIngress(staged, new byte[] {2}, null, ignored -> {}));
        assertThrows(
                IllegalStateException.class,
                () -> owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged)));
    }

    @Test
    void authoritySelectedRootPublishesHiddenActorButKeepsAdmissionClosed() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();

        var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
        owner.publishHidden(backlog, 0);

        assertTrue(backend.visible);
        assertFalse(backend.admitted);
        assertEquals(List.of("prepare", "publish"), backend.operations);
        owner.openAdmission(staged);
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        assertEquals(List.of("prepare", "publish", "open"), backend.operations);
    }

    @Test
    void canonicalReplayerPreservesActorReplyCapabilityBeforePublication() {
        FakeBackend backend = new FakeBackend();
        backend.replayReply = Optional.of(new byte[] {9, 8});
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] accepted =
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 23, "actor.request", Map.of("trace", "a"), new byte[] {1, 2});
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        "actor-a",
                        7,
                        11,
                        true,
                        new byte[] {4, 5},
                        List.of(new ZLinkSerialExecutionQueue.QueuedRecord(5, accepted)));
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        AtomicReference<ZLinkActorAcceptedJournal.Record> relayed = new AtomicReference<>();

        var replayer =
                new ZLinkAcceptedJournalReplayer(
                        record ->
                                CompletableFuture.failedFuture(
                                        new AssertionError(
                                                "standalone Actor must not replay Spot")),
                        record -> owner.replayActor(staged, record),
                        new ZLinkAcceptedJournalReplayer.ReplyRelay() {
                            @Override
                            public CompletionStage<Void> completeSpot(
                                    ZLinkSpotAcceptedJournal.Record record,
                                    long acceptedSequence,
                                    List<byte[]> reply) {
                                return CompletableFuture.failedFuture(
                                        new AssertionError("standalone Actor must not relay Spot"));
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
                                assertArrayEquals(new byte[] {9, 8}, reply.orElseThrow());
                                relayed.set(record);
                                return CompletableFuture.completedFuture(null);
                            }
                        });

        var backlog = owner.closeDurableBacklog(staged, root, replayer);
        assertNull(relayed.get());
        owner.publishHidden(backlog, 0);
        owner.openAdmission(staged);
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();

        assertNotNull(relayed.get());
        assertTrue(backend.visible);
        assertTrue(backend.admitted);
        assertEquals(List.of("prepare", "publish", "open", "replay"), backend.operations);
    }

    @Test
    void authoritySelectedRootCannotReplaceCapturedApplicationState() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        var staged =
                owner.stage(
                                request(relocationId, true),
                                ZLinkCanonicalActorRelocationEnvelope.encode(
                                        relocationId,
                                        "actor-a",
                                        7,
                                        11,
                                        true,
                                        new byte[] {4, 5},
                                        List.of()))
                        .toCompletableFuture()
                        .join();
        byte[] changed =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {9}, List.of());

        assertThrows(
                IllegalArgumentException.class,
                () -> owner.closeDurableBacklog(staged, changed, actorReplayer(owner, staged)));
        assertFalse(backend.visible);
    }

    @Test
    void rootMustMatchActorAuthorityFence() {
        FakeBackend backend = new FakeBackend();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 8, 11, false, new byte[0], List.of());

        assertThrows(
                IllegalArgumentException.class,
                () -> owner.stage(request(relocationId, true), root));
        assertTrue(backend.operations.isEmpty());
    }

    private static ZLinkStandaloneActorRelocationStagingOwner.Request request(
            UUID relocationId, boolean restoreSnapshot) {
        return new ZLinkStandaloneActorRelocationStagingOwner.Request(
                relocationId, "actor-a", "player", 7, 11, restoreSnapshot, "target-entry");
    }

    private static ZLinkUserSpotAggregateStagingOwner.JournalReplayer actorReplayer(
            ZLinkStandaloneActorRelocationStagingOwner owner,
            ZLinkStandaloneActorRelocationStagingOwner.Staged staged) {
        return (laneId, record) ->
                owner.replayActor(staged, ZLinkActorAcceptedJournal.decode(record.payload()))
                        .thenApply(ignored -> null);
    }

    private static final class FakeBackend
            implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final List<String> operations = new ArrayList<>();
        private boolean visible;
        private boolean admitted;
        private boolean discarded;
        private Optional<byte[]> replayReply;
        private int replayFailuresRemaining;
        private int rejectAdmissionsRemaining;
        private CompletionStage<Void> discardResult = CompletableFuture.completedFuture(null);

        @Override
        public <T> CompletionStage<T> admitApplicationJob(
                java.util.function.Supplier<CompletionStage<T>> turn) {
            if (rejectAdmissionsRemaining-- > 0) {
                throw new IllegalStateException("backlog admission rejected");
            }
            return turn.get();
        }

        @Override
        public CompletionStage<Object> prepare(
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                byte[] state,
                ZLinkRelocationCancellation cancellation) {
            operations.add("prepare");
            assertArrayEquals(request.restoreSnapshot() ? new byte[] {4, 5} : new byte[0], state);
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
            if (replayFailuresRemaining-- > 0) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("first replay failed"));
            }
            return CompletableFuture.completedFuture(replayReply);
        }

        @Override
        public void publish(
                Object actor, ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            operations.add("publish");
            visible = true;
        }

        @Override
        public void openAdmission(Object actor) {
            operations.add("open");
            admitted = true;
        }

        @Override
        public CompletionStage<Void> discard(
                Object actor, ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            operations.add("discard");
            discarded = true;
            return discardResult;
        }
    }
}
