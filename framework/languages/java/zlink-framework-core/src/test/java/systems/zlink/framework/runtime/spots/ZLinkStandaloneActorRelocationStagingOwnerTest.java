package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn;
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
    @ParameterizedTest
    @ValueSource(strings = {"saved", "relayed", "temporary"})
    void hiddenBacklogIsAdoptedByActorFifoBeforeFinalPublicationOpensHandlers(String origin) {
        var tasks = new ZLinkManualExecutor();
        var moving = new CompletableFuture<Void>();
        var fifoDispatch = tasks.actorDispatch("actor-a", "prepared", moving);
        var queue = fifoDispatch.activation().relocationLane();
        var handlerTerminal = new CompletableFuture<Optional<byte[]>>();
        var events = new ArrayList<String>();
        FakeBackend backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<QueuedTurn> prepareIngress(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
                        return CompletableFuture.completedFuture(fifoDispatch);
                    }

                    @Override
                    public CompletionStage<Void> enqueueIngress(
                            QueuedTurn dispatch,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            byte[] record,
                            java.util.function.Supplier<CompletionStage<Void>> turn,
                            CompletableFuture<Void> admission) {
                        return queue.enqueueRelocatableLazyRecord(
                                () -> record, record.length, turn, () -> {}, admission);
                    }

                    @Override
                    public CompletionStage<Optional<byte[]>> replay(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            ZLinkActorAcceptedJournal.Record record) {
                        events.add("handler");
                        return handlerTerminal;
                    }

                    @Override
                    public void openAdmission(Object actor) {
                        events.add("route-and-normalizer-complete");
                        moving.complete(null);
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        var relocationId = UUID.randomUUID();
        byte[] record =
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 1, "actor.request", Map.of(), new byte[] {1});
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        "actor-a",
                        7,
                        11,
                        true,
                        new byte[] {4, 5},
                        origin.equals("saved")
                                ? List.of(new ZLinkSerialExecutionQueue.QueuedRecord(1, record))
                                : List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        var failure = new AtomicReference<Throwable>();
        if (origin.equals("temporary")) owner.acceptIngress(staged, record, null, failure::set);
        if (origin.equals("relayed"))
            owner.stageRelayedRecord(staged, record).toCompletableFuture().join();
        var backlog =
                owner.closeDurableBacklog(
                        staged,
                        root,
                        (lane, queued) ->
                                owner.replayActor(
                                                staged,
                                                ZLinkActorAcceptedJournal.decode(queued.payload()))
                                        .whenComplete(
                                                (reply, error) -> {
                                                    if (error != null) failure.set(error);
                                                })
                                        .thenApply(ignored -> null));
        owner.publishHidden(backlog, 12);
        try {
            var adopted = owner.drainDurableBacklog(backlog).toCompletableFuture();
            tasks.drain();
            assertTrue(
                    adopted.isDone() && !adopted.isCompletedExceptionally(),
                    "hidden backlog admission must finish without opening handlers");
            assertEquals(List.of(), events);
            owner.openAdmission(staged);
            tasks.drain();
            assertEquals(List.of("route-and-normalizer-complete", "handler"), events);
            assertFalse(handlerTerminal.isDone());
            var terminalFailure = new IllegalStateException("handler failed after publication");
            handlerTerminal.completeExceptionally(terminalFailure);
            tasks.drain();
            assertSame(terminalFailure, failure.get());
        } finally {
            moving.complete(null);
            handlerTerminal.complete(Optional.empty());
            tasks.drain();
            queue.close();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void failedPreparationRetainsFrozenBacklog(boolean direct) throws Exception {
        var rejected = new IllegalStateException("prepared incarnation unavailable");
        FakeBackend backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<QueuedTurn> prepareIngress(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request) {
                        return CompletableFuture.failedFuture(rejected);
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        var id = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        id, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(id, true), root).toCompletableFuture().join();
        var observed = new AtomicReference<Throwable>();
        owner.acceptIngress(
                staged,
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                null,
                observed::set);
        CompletionStage<Void> adopted;
        Object retained;
        if (direct) {
            var backlog = owner.closeDirectJoinIngress(staged, root);
            owner.publishDirectJoinHidden(backlog, 12);
            retained = backlog;
            adopted = owner.replayDirectJoin(backlog, actorReplayer(owner, staged));
        } else {
            var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
            owner.publishHidden(backlog, 12);
            retained = backlog;
            adopted = owner.drainDurableBacklog(backlog);
        }
        assertSame(
                rejected,
                assertThrows(
                                java.util.concurrent.CompletionException.class,
                                () -> adopted.toCompletableFuture().join())
                        .getCause());
        assertNull(observed.get(), "preparation failure must retain the unadopted ingress owner");
        var field = staged.getClass().getDeclaredField(direct ? "directBacklog" : "durableBacklog");
        field.setAccessible(true);
        assertSame(
                retained,
                field.get(staged),
                "unadopted frozen payload must retain its existing backlog owner");
    }

    @Test
    void backendFailureBeforeQueueAdmissionSettlesPublicationAndIngress() {
        var rejected = new IllegalStateException("queue target unavailable");
        FakeBackend backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<Void> enqueueIngress(
                            QueuedTurn dispatch,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            byte[] record,
                            java.util.function.Supplier<CompletionStage<Void>> turn,
                            CompletableFuture<Void> admission) {
                        return CompletableFuture.failedFuture(rejected);
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        var relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(relocationId, true), root).toCompletableFuture().join();
        var failure = new AtomicReference<Throwable>();
        owner.acceptIngress(
                staged,
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                null,
                failure::set);
        var backlog = owner.closeDurableBacklog(staged, root, actorReplayer(owner, staged));
        owner.publishHidden(backlog, 12);
        var adopted = owner.drainDurableBacklog(backlog).toCompletableFuture();
        assertTrue(adopted.isCompletedExceptionally());
        assertSame(
                rejected,
                assertThrows(java.util.concurrent.CompletionException.class, adopted::join)
                        .getCause());
        assertSame(rejected, failure.get());
    }

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
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        owner.openAdmission(staged);
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
        owner.replayDirectJoin(replay, actorReplayer(owner, staged)).toCompletableFuture().join();
        owner.openAdmission(staged);
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
        assertThrows(
                java.util.concurrent.CompletionException.class,
                () -> owner.drainDurableBacklog(backlog).toCompletableFuture().join());
        owner.openAdmission(staged);
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
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        owner.openAdmission(staged);
        assertTrue(backend.admitted);
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
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        owner.openAdmission(staged);
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
                                    List<byte[]> reply,
                                    Throwable failure) {
                                return CompletableFuture.failedFuture(
                                        new AssertionError("standalone Actor must not relay Spot"));
                            }

                            @Override
                            public CompletionStage<Void> completeActor(
                                    ZLinkActorAcceptedJournal.Record record,
                                    long acceptedSequence,
                                    Optional<byte[]> reply,
                                    Throwable failure) {
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
        owner.drainDurableBacklog(backlog).toCompletableFuture().join();
        owner.openAdmission(staged);

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

    private static class FakeBackend implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        private final List<String> operations = new ArrayList<>();
        private boolean visible;
        private boolean admitted;
        private boolean discarded;
        private Optional<byte[]> replayReply;
        private int replayFailuresRemaining;
        private int rejectAdmissionsRemaining;
        private CompletionStage<Void> discardResult = CompletableFuture.completedFuture(null);

        private final CompletableFuture<Void> moving = new CompletableFuture<>();
        private final ZLinkManualExecutor executor = new ZLinkManualExecutor();

        @Override
        public CompletionStage<QueuedTurn> prepareIngress(
                Object actor, ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            return CompletableFuture.completedFuture(
                    executor.actorDispatch(request.actorId(), actor, moving));
        }

        @Override
        public CompletionStage<Void> enqueueIngress(
                QueuedTurn dispatch,
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                byte[] record,
                java.util.function.Supplier<CompletionStage<Void>> turn,
                CompletableFuture<Void> admission) {
            if (rejectAdmissionsRemaining-- > 0)
                throw new IllegalStateException("backlog admission rejected");
            return dispatch.target()
                    .executeActorLazyRecord(
                            dispatch.activation(),
                            () -> record,
                            record.length,
                            turn,
                            () -> {},
                            admission);
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
            moving.complete(null);
            executor.drain();
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
