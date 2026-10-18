package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.drain.AsyncDrainProbe;
import systems.zlink.framework.runtime.mesh.ZLinkActivationAdmission;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiFunction;
import java.util.function.Supplier;

final class ZLinkStandaloneActorRelocationStagingOwnerTest {
    @ParameterizedTest
    @ValueSource(strings = {"saved", "relayed", "temporary"})
    void hiddenBacklogIsAdoptedByActorFifoBeforeFinalPublicationOpensHandlers(String origin) {
        var handlerTerminal = new CompletableFuture<Optional<byte[]>>();
        var events = new ArrayList<String>();
        FakeBackend backend =
                new FakeBackend() {
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
                        ((ZLinkActorRelocationTestQueue) actor).open();
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
            assertTrue(
                    adopted.isDone() && !adopted.isCompletedExceptionally(),
                    "hidden backlog admission must finish without opening handlers");
            assertEquals(List.of(), events);
            owner.openAdmission(staged);
            assertEquals(List.of("route-and-normalizer-complete", "handler"), events);
            assertFalse(handlerTerminal.isDone());
            var terminalFailure = new IllegalStateException("handler failed after publication");
            handlerTerminal.completeExceptionally(terminalFailure);
            assertSame(
                    terminalFailure,
                    assertThrows(
                                    java.util.concurrent.CompletionException.class,
                                    () -> adopted.join().toCompletableFuture().join())
                            .getCause());
            if (!origin.equals("temporary")) assertSame(terminalFailure, failure.get());
        } finally {
            handlerTerminal.complete(Optional.empty());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void failedPreparationRetainsFrozenBacklog(boolean direct) throws Exception {
        var rejected = new IllegalStateException("prepared incarnation unavailable");
        FakeBackend backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                            BiFunction<
                                            ZLinkSerialExecutionQueue.QueuedRecord,
                                            CompletableFuture<Void>,
                                            Supplier<CompletionStage<Void>>>
                                    replay) {
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
        CompletionStage<?> adopted;
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

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void timerPreparationFailureDiscardsPreparedActorBeforeReleasingAdmission(boolean synchronous) {
        var preparationFailure = new IllegalStateException("timer preparation failed");
        var discardFailure = new IllegalStateException("discard failed");
        FakeBackend backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<Void> stageTimers(
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            byte[] envelope) {
                        if (synchronous) throw preparationFailure;
                        return CompletableFuture.failedFuture(preparationFailure);
                    }
                };
        var discard = new CompletableFuture<Void>();
        backend.discardResult = discard;
        var admission = new ZLinkActivationAdmission(1);
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend, admission);
        var relocationId = UUID.randomUUID();
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var result = owner.stage(request(relocationId, true), root).toCompletableFuture();
        assertEquals(List.of("prepare", "discard"), backend.operations);
        assertFalse(
                result.isDone(), "the prepared resource must settle before admission is released");
        assertEquals(1, admission.snapshot().active());
        discard.completeExceptionally(discardFailure);
        var terminal = assertThrows(java.util.concurrent.CompletionException.class, result::join);
        assertSame(preparationFailure, terminal.getCause());
        assertArrayEquals(new Throwable[] {discardFailure}, preparationFailure.getSuppressed());
        assertEquals(0, admission.snapshot().active());
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
        owner.openAdmission(staged);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () ->
                        owner.drainDurableBacklog(backlog)
                                .thenCompose(terminal -> terminal)
                                .toCompletableFuture()
                                .join());
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
                                .thenCompose(terminal -> terminal)
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
                () ->
                        owner.drainDurableBacklog(backlog)
                                .thenCompose(terminal -> terminal)
                                .toCompletableFuture()
                                .join());
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
        owner.drainDurableBacklog(backlog)
                .thenCompose(terminal -> terminal)
                .toCompletableFuture()
                .join();
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
        owner.drainDurableBacklog(backlog)
                .thenCompose(terminal -> terminal)
                .toCompletableFuture()
                .join();
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
        owner.openAdmission(staged);
        owner.drainDurableBacklog(backlog)
                .thenCompose(terminal -> terminal)
                .toCompletableFuture()
                .join();

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

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.ValueSource(booleans = {false, true})
    void prefixAdmissionPrecedesPublicationAndWaitsForExistingLifecycle(boolean nonempty) {
        var backend = new FakeBackend();
        backend.replayReply = Optional.empty();
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID id = UUID.randomUUID();
        var root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        id, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(id, true), root).toCompletableFuture().join();
        if (nonempty)
            assertTrue(
                    owner.acceptIngress(
                            staged,
                            ZLinkAcceptedJournalTestRecords.actor(
                                    "actor-a", 1, "actor.request", Map.of(), new byte[] {1}),
                            null,
                            ignored -> fail("replay must succeed")));
        var replay = owner.closeDirectJoinIngress(staged, root);
        var terminal =
                owner.replayDirectJoin(replay, actorReplayer(owner, staged))
                        .toCompletableFuture()
                        .join();
        assertEquals(
                List.of("prepare"),
                backend.operations,
                "admitted prefix cannot execute before publication/lifecycle");
        owner.publishDirectJoinHidden(replay, 12);
        assertEquals(List.of("prepare", "publish"), backend.operations);
        owner.openAdmission(staged);
        terminal.toCompletableFuture().join();
        assertEquals(
                nonempty
                        ? List.of("prepare", "publish", "open", "replay")
                        : List.of("prepare", "publish", "open"),
                backend.operations);
    }

    @Test
    void eachPreparedFixtureActorOwnsItsQueueAndDiscardRunsNoHandler() throws Exception {
        var backend = new FakeBackend();
        var request = request(UUID.randomUUID(), false);
        var first = backend.prepare(request, new byte[0], () -> false).toCompletableFuture().join();
        var second =
                backend.prepare(request, new byte[0], () -> false).toCompletableFuture().join();
        assertNotSame(first, second);
        var calls = new ArrayList<String>();
        var records = List.of(new ZLinkSerialExecutionQueue.QueuedRecord(1, new byte[] {1}));
        var firstEntries =
                backend.admitBacklogPrefix(
                                first,
                                request,
                                records,
                                (record, terminal) ->
                                        () -> {
                                            calls.add("discarded");
                                            return CompletableFuture.completedFuture(null);
                                        })
                        .toCompletableFuture()
                        .join();
        var secondEntries =
                backend.admitBacklogPrefix(
                                second,
                                request,
                                records,
                                (record, terminal) ->
                                        () -> {
                                            calls.add("second");
                                            return CompletableFuture.completedFuture(null);
                                        })
                        .toCompletableFuture()
                        .join();
        var discarded = backend.discard(first, request).toCompletableFuture();
        assertTrue(discarded.isDone());
        discarded.join();
        assertTrue(firstEntries.getFirst().isCancelled());
        assertTrue(calls.isEmpty());
        assertFixtureReleased(first);
        assertFalse(secondEntries.getFirst().isDone());
        backend.openAdmission(second);
        assertTrue(secondEntries.getFirst().isDone());
        secondEntries.getFirst().join();
        assertEquals(List.of("second"), calls);
        assertFixtureReleased(second);
    }

    @Test
    void fixtureFactoryFailureSettlesBarrierAndClosesItsHostOwner() throws Exception {
        var helper = new ZLinkActorRelocationTestQueue();
        var original = new IllegalArgumentException("fixture factory failed");
        var failed =
                helper.admit(
                                List.of(
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1, new byte[] {1})),
                                (record, terminal) -> {
                                    throw original;
                                })
                        .toCompletableFuture();
        assertTrue(failed.isCompletedExceptionally());
        assertSame(
                original,
                assertThrows(java.util.concurrent.CompletionException.class, failed::join)
                        .getCause());
        assertFixtureReleased(helper);
    }

    private static void assertFixtureReleased(Object actor) throws Exception {
        var helper = (ZLinkActorRelocationTestQueue) actor;
        var fifoField = ZLinkActorRelocationTestQueue.class.getDeclaredField("fifo");
        var jobsField = ZLinkActorRelocationTestQueue.class.getDeclaredField("jobs");
        fifoField.setAccessible(true);
        jobsField.setAccessible(true);
        var fifo = (ZLinkSerialExecutionQueue) fifoField.get(helper);
        var jobs = (ZLinkApplicationJobQueue) jobsField.get(helper);
        try {
            var quiet = fifo.awaitQuiescence().toCompletableFuture();
            assertTrue(quiet.isDone());
            quiet.join();
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            var closed = jobs.acquire(ZLinkApplicationJobQueue.Origin.LOCAL).toCompletableFuture();
            if (closed.isDone() && !closed.isCompletedExceptionally()) closed.join().close();
            assertTrue(
                    closed.isCompletedExceptionally(),
                    "fixture host owner must close after FIFO quiescence");
        } finally {
            jobs.close();
            fifo.close();
        }
    }

    @org.junit.jupiter.api.RepeatedTest(20)
    void canonicalPrefixIsOwnedBeforeFirstHandlerCanBeginNextHandoff() {
        var executor = new PrefixExecutor();
        var serial =
                new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.actorDelivery());
        var jobs =
                new ZLinkApplicationJobQueue(
                        ZLinkApplicationJobQueueProfile.BALANCED,
                        OptionalLong.of(1),
                        new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
        var events = new ArrayList<Integer>();
        var captured = new AtomicReference<ZLinkSerialExecutionQueue.RelocationSeal>();
        var p1Terminal = new CompletableFuture<Optional<byte[]>>();
        var qAdmission = new AtomicReference<CompletableFuture<Void>>();
        var backend =
                new FakeBackend() {
                    @Override
                    public CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                            BiFunction<
                                            ZLinkSerialExecutionQueue.QueuedRecord,
                                            CompletableFuture<Void>,
                                            Supplier<CompletionStage<Void>>>
                                    replay) {
                        return serial.admitDurablePrefix(records, replay, jobs)
                                .thenApply(
                                        terminal -> {
                                            // The same semantic boundary as the red fixture: Q
                                            // arrives after prefix
                                            // admission starts and before P1 runs. It must really
                                            // be accepted.
                                            var accepted = new CompletableFuture<Void>();
                                            qAdmission.set(
                                                    jobs.acquire(
                                                                    ZLinkApplicationJobQueue.Origin
                                                                            .REMOTE)
                                                            .thenCompose(
                                                                    permit -> {
                                                                        try (var scope =
                                                                                ZLinkApplicationJobContext
                                                                                        .enter(
                                                                                                permit)) {
                                                                            return serial
                                                                                    .enqueueRelocatable(
                                                                                            ZLinkAcceptedJournalTestRecords
                                                                                                    .actor(
                                                                                                            "actor-a",
                                                                                                            3,
                                                                                                            "actor.request",
                                                                                                            Map
                                                                                                                    .of(),
                                                                                                            new byte
                                                                                                                    [] {
                                                                                                                3
                                                                                                            }),
                                                                                            () -> {
                                                                                                ZLinkApplicationJobContext
                                                                                                        .beforeFirstApplicationInstruction();
                                                                                                events
                                                                                                        .add(
                                                                                                                3);
                                                                                                return CompletableFuture
                                                                                                        .completedFuture(
                                                                                                                null);
                                                                                            },
                                                                                            () -> {},
                                                                                            accepted);
                                                                        } finally {
                                                                            permit
                                                                                    .abandonReservation();
                                                                        }
                                                                    })
                                                            .toCompletableFuture());
                                            assertTrue(
                                                    accepted.isDone(),
                                                    "Q must be accepted before P1 starts");
                                            accepted.join();
                                            return terminal;
                                        });
                    }

                    @Override
                    public CompletionStage<Optional<byte[]>> replay(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            ZLinkActorAcceptedJournal.Record record) {
                        int id = record.payload()[0];
                        ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
                        events.add(id);
                        if (id == 1) {
                            captured.set(
                                    serial.trySealRelocation(
                                                    serial.captureActiveTurnSealHandle()
                                                            .orElseThrow())
                                            .toCompletableFuture()
                                            .join()
                                            .orElseThrow());
                            return p1Terminal;
                        }
                        return CompletableFuture.completedFuture(Optional.empty());
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        UUID id = UUID.randomUUID();
        var root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        id, "actor-a", 7, 11, true, new byte[] {4, 5}, List.of());
        var staged = owner.stage(request(id, true), root).toCompletableFuture().join();
        for (int value : List.of(1, 2)) {
            assertTrue(
                    owner.acceptIngress(
                            staged,
                            ZLinkAcceptedJournalTestRecords.actor(
                                    "actor-a",
                                    value,
                                    "actor.request",
                                    Map.of(),
                                    new byte[] {(byte) value}),
                            null,
                            ignored -> {}));
        }
        var replay = owner.closeDirectJoinIngress(staged, root);
        var admitted =
                owner.replayDirectJoin(replay, actorReplayer(owner, staged))
                        .toCompletableFuture()
                        .join();
        assertEquals(List.of(), events);
        owner.publishDirectJoinHidden(replay, 12);
        owner.openAdmission(staged);
        var finished = admitted.toCompletableFuture();
        try {
            executor.drain();
            assertNotNull(captured.get());
            assertFalse(p1Terminal.isDone(), "next handoff must own the suffix before P1 terminal");
            assertEquals(
                    List.of(2, 3),
                    captured.get().captured().stream()
                            .map(
                                    record ->
                                            (int)
                                                    ZLinkActorAcceptedJournal.decode(
                                                                    record.payload())
                                                            .payload()[0])
                            .toList(),
                    "next handoff must take P2 then Q; P2 cannot remain outside FIFO waiting for Q's permit");
        } finally {
            p1Terminal.complete(Optional.empty());
            executor.drain();
            if (captured.get() != null)
                assertTrue(serial.abortRelocation(captured.get()).toCompletableFuture().join());
            executor.drain();
            finished.join();
            if (qAdmission.get() != null) qAdmission.get().join();
            serial.close();
            assertEquals(0, jobs.snapshot().permitsInUse());
            assertEquals(0, jobs.snapshot().capacityWaiters());
            jobs.close();
        }
    }

    private static final class PrefixExecutor extends java.util.concurrent.AbstractExecutorService {
        private final java.util.ArrayDeque<Runnable> pending = new java.util.ArrayDeque<>();
        private boolean shutdown;

        public void execute(Runnable command) {
            pending.addLast(command);
        }

        void drain() {
            while (!pending.isEmpty()) pending.removeFirst().run();
        }

        public void shutdown() {
            shutdown = true;
        }

        public List<Runnable> shutdownNow() {
            throw new UnsupportedOperationException();
        }

        public boolean isShutdown() {
            return shutdown;
        }

        public boolean isTerminated() {
            return shutdown && pending.isEmpty();
        }

        public boolean awaitTermination(long ignored, java.util.concurrent.TimeUnit unit) {
            return isTerminated();
        }
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

        @Override
        public CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                Object actor,
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                BiFunction<
                                ZLinkSerialExecutionQueue.QueuedRecord,
                                CompletableFuture<Void>,
                                Supplier<CompletionStage<Void>>>
                        replay) {
            return ((ZLinkActorRelocationTestQueue) actor)
                    .admit(
                            records,
                            (record, terminal) -> {
                                var operation = replay.apply(record, terminal);
                                if (rejectAdmissionsRemaining-- > 0)
                                    return () ->
                                            CompletableFuture.failedFuture(
                                                    new IllegalStateException(
                                                            "backlog admission rejected"));
                                return operation;
                            });
        }

        @Override
        public CompletionStage<Void> enqueueIngress(
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                byte[] acceptedJournalRecord,
                java.util.function.Supplier<CompletionStage<Void>> turn) {
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
            return CompletableFuture.completedFuture(new ZLinkActorRelocationTestQueue());
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
            ((ZLinkActorRelocationTestQueue) actor).open();
        }

        @Override
        public CompletionStage<Void> discard(
                Object actor, ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            operations.add("discard");
            discarded = true;
            return ((ZLinkActorRelocationTestQueue) actor)
                    .discard()
                    .thenCompose(ignored -> discardResult);
        }
    }
}
