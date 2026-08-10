package systems.zlink.framework.runtime.actors;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkSessionActorBindingContractTest {
    private static final RoutingId SESSION = RoutingId.from("session-a");
    private static final RoutingId NODE_A = RoutingId.from("actor-node-a");
    private static final RoutingId NODE_B = RoutingId.from("actor-node-b");
    private static final RoutingId NODE_C = RoutingId.from("actor-node-c");
    private static final String MESH = "game";
    private static final long BINDING_GENERATION = 6_001;

    @Test
    void relayUsesTheStoredBindingWithoutHiddenRebind() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A)).toCompletableFuture().join();
        assertEquals(1, stream.binds.size());

        ZLinkSessionActorsRuntime.enterRelayDispatch(header("Play"));
        try {
            actor.relay(ZLinkMessage.fromEncoded(
                ZLinkEncodedPayload.from("payload".getBytes(StandardCharsets.UTF_8)),
                new RawSerializer())).toCompletableFuture().join();
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }

        assertEquals(List.of("actor-1"), stream.binds);
        assertEquals(1, stream.relays.size());
    }

    @Test
    void nativeBindWaitsForTheTargetSessionContextAcknowledgement() {
        FakeStream stream = new FakeStream();
        CompletableFuture<List<Message>> acknowledgement =
            new CompletableFuture<>();
        stream.pendingBoundRequest = acknowledgement;
        ZLinkSessionActorsRuntime runtime = runtime(stream);

        var binding = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A));

        assertFalse(binding.toCompletableFuture().isDone());
        acknowledgement.complete(List.of(Message.from(new byte[0])));
        binding.toCompletableFuture().join();
    }

    @Test
    void staleStoredRouteFailsOnceWithoutLookupOrHiddenRetry() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A)).toCompletableFuture().join();
        stream.relayFailure = SubmitResult.NOT_FOUND;
        int relaysBeforeFailure = stream.relays.size();

        ZLinkSessionActorsRuntime.enterRelayDispatch(header("Play"));
        try {
            CompletionException completion = assertThrows(
                CompletionException.class,
                () -> actor.relay(ZLinkMessage.empty()).toCompletableFuture().join());
            ZLinkFrameworkException error =
                (ZLinkFrameworkException) completion.getCause();
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                error.kind());
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }

        assertEquals(1, stream.binds.size());
        assertEquals(relaysBeforeFailure + 1, stream.relays.size());
    }

    @Test
    void rebindFencesTheStaleTokenAndNewGenerationNeedsExplicitBind() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor oldBinding = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A)).toCompletableFuture().join();
        ZLinkSessionActor replacement = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_B)).toCompletableFuture().join();

        oldBinding.notifyDisconnected().toCompletableFuture().join();
        assertTrue(stream.unbinds.isEmpty());
        assertEquals(0, stream.disconnectNotifications,
            "retired binding cleanup is not part of replacement completion");
        assertEquals(NODE_B, replacement.ref().nodeRid());
        assertEquals(7, replacement.ref().objectGeneration());

        ZLinkSessionActor newIncarnation = runtime.bind(
            new ActorRef("actor-1", 8, MESH, NODE_A)).toCompletableFuture().join();
        replacement.notifyDisconnected().toCompletableFuture().join();
        assertTrue(stream.unbinds.isEmpty());
        assertEquals(0, stream.disconnectNotifications);
        assertEquals(8, newIncarnation.ref().objectGeneration());
        assertEquals(List.of(newIncarnation), runtime.bound());
    }

    @Test
    void rebindCompletesAfterInstallingCurrentBindingWithoutWaitingForRetiredCleanup() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        CompletableFuture<List<Message>> previousCallback =
            new CompletableFuture<>();
        stream.pendingDisconnectNotification = previousCallback;

        CompletionStage<ZLinkSessionActor> replacement = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_B));

        assertTrue(replacement.toCompletableFuture().isDone());
        assertEquals(0, stream.disconnectNotifications);
        assertEquals(2, stream.binds.size(),
            "the new exact identity is installed before retired cleanup");
        previousCallback.complete(
            List.of(Message.from(new byte[0])));
        assertEquals(NODE_B,
            replacement.toCompletableFuture().join().ref().nodeRid());
        assertEquals(1, runtime.bound().size());
    }

    @Test
    void physicalDisconnectUsesTheLifecycleCallbackDeadline() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        stream.pendingDisconnectNotification = new CompletableFuture<>();

        CompletionStage<Void> disconnected = runtime.notifyDisconnectedAll(
            Duration.ofMillis(25));

        assertThrows(
            CompletionException.class,
            () -> disconnected.toCompletableFuture().join());
        assertEquals(1, stream.disconnectNotifications);
        assertTrue(runtime.bound().isEmpty());
    }

    @Test
    void nativeRebindUpdatesTheStoredRouteForTheNextRelocationFence() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        runtime.recordNativeRebind(
            (ZLinkBoundActor) actor,
            new ZLinkBackendActorRef(NODE_B, "actor-1", 7));

        assertEquals(NODE_B, actor.ref().nodeRid());
        var relocation = new ZLinkServiceM6BWireCodec.RelocationIdentity(30, 31);
        var sealed = runtime.applyRelocationSealCommand(
                seal(relocation, 7, NODE_B, 9))
            .toCompletableFuture().join();
        runtime.applyRelocationRouteCommand(
                route(
                    relocation,
                    sealed.lastAcceptedSessionSequence(),
                    9,
                    10,
                    NODE_A,
                    4))
            .toCompletableFuture().join();
        assertEquals(NODE_A, actor.ref().nodeRid());
    }

    @Test
    void physicalDisconnectUsesExactSnapshotAllSettledAndAlwaysCleansBindings() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-a", 1, MESH, NODE_A)).toCompletableFuture().join();
        runtime.bind(new ActorRef("actor-b", 1, MESH, NODE_A)).toCompletableFuture().join();
        stream.deferUnbind = true;

        var first = runtime.notifyDisconnectedAll();
        var duplicate = runtime.notifyDisconnectedAll();
        //  The unbind submit is deliberately dispatched off the completing
        //  thread, so wait for both to arrive instead of assuming they were
        //  issued inline.
        awaitUnbinds(stream, "actor-a", "actor-b");
        assertEquals(Set.of("actor-a", "actor-b"), Set.copyOf(stream.unbinds));
        stream.pendingUnbinds.get("actor-a").completeExceptionally(
            new IllegalStateException("forced unbind failure"));
        stream.pendingUnbinds.get("actor-b").complete(null);

        assertThrows(
            CompletionException.class,
            () -> first.toCompletableFuture().join());
        assertThrows(
            CompletionException.class,
            () -> duplicate.toCompletableFuture().join());
        assertEquals(Set.of("actor-a", "actor-b"), Set.copyOf(stream.unbinds));
        assertTrue(runtime.bound().isEmpty());
    }

    @Test
    void logicalDisconnectNotifiesOnlyTheSelectedActorOnTheLiveSession() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor first = runtime.bind(
            new ActorRef("actor-a", 1, MESH, NODE_A)).toCompletableFuture().join();
        ZLinkSessionActor second = runtime.bind(
            new ActorRef("actor-b", 1, MESH, NODE_A)).toCompletableFuture().join();

        first.notifyDisconnected().toCompletableFuture().join();

        awaitUnbinds(stream, "actor-a");
        assertEquals(List.of("actor-a"), stream.unbinds);
        assertEquals(List.of(second), runtime.bound());
        assertFalse(stream.closed);
    }

    @Test
    void relocationCommandUpdatesOnlyTheExactStoredRouteBeforeAck() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        var relocation = new ZLinkServiceM6BWireCodec.RelocationIdentity(32, 33);
        var sealed = runtime.applyRelocationSealCommand(
                seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        runtime.applyRelocationRouteCommand(
                route(
                    relocation,
                    sealed.lastAcceptedSessionSequence(),
                    9,
                    10,
                    NODE_B,
                    4))
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(7, actor.ref().objectGeneration());
        var stale = new ZLinkServiceM6BWireCodec.RelocationIdentity(34, 35);
        assertThrows(
            CompletionException.class,
            () -> runtime.applyRelocationSealCommand(
                    seal(stale, 7, NODE_A, 9))
                .toCompletableFuture().join(),
            "the production command 42 path must reject the stale source route");
    }

    @Test
    void command44ChangesOnlyTheExactBindingAndProducesCommand45Ack() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
        var coordinator = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4");
        var session = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, BINDING_GENERATION);
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        var command = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                relocation,
                coordinator,
                ZLinkServiceM6BWireCodec
                    .RelocationRole.TARGET,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
                session,
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.COMMIT,
                9, 10, NODE_B, 4, 0);

        var ack = runtime.applyRelocationRouteCommand(command)
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(7, actor.ref().objectGeneration());
        assertEquals(10, ack.currentAuthorityOwnerGeneration());
        assertEquals(0, ack.lastAcceptedSessionSequence());
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, ack.result());
        //  Spec 20 §5: the session owner must answer a retransmitted command
        //  44 instead of failing the source-fence CAS, or a lost command 45
        //  leaves the target retrying forever. The cached terminal preserves
        //  the original fence even after later ingress advances high-water.
        var replay = runtime.applyRelocationRouteCommand(command)
            .toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .ALREADY_APPLIED, replay.result());
        assertEquals(ack, new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            replay.relocation(), replay.coordinator(), replay.actor(),
            replay.session(), replay.action(),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
            replay.currentAuthorityOwnerGeneration(),
            replay.lastAcceptedSessionSequence()));
        CompletionException conflict = assertThrows(CompletionException.class,
            () -> runtime.applyRelocationRouteCommand(route(relocation, 1))
                .toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ((ZLinkFrameworkException) conflict.getCause()).kind());
    }

    @Test
    void command44KeepsTheGateSealedUntilOneNativePreparationCommits() {
        FakeStream stream = new FakeStream();
        stream.deferUnbind = true;
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(
                seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> firstHeld = relay(actor, "held-1");
        CompletionStage<Void> secondHeld = relay(actor, "held-2");
        var command = route(relocation, 0);

        CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
            first = runtime.applyRelocationRouteCommand(command);
        CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationRouted>
            duplicate = runtime.applyRelocationRouteCommand(command);
        awaitUnbinds(stream, "actor-1");

        assertSame(first, duplicate,
            "an identical command 44 shares native route preparation");
        assertFalse(first.toCompletableFuture().isDone());
        assertFalse(firstHeld.toCompletableFuture().isDone());
        assertEquals(NODE_A, actor.ref().nodeRid(),
            "logical route changes only in the owner-lock commit transition");
        CompletionException conflict = assertThrows(CompletionException.class,
            () -> runtime.applyRelocationRouteCommand(
                    new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                        command.relocation(), command.coordinator(),
                        command.senderRole(), command.actor(), command.session(),
                        command.action(),
                        command.previousAuthorityOwnerGeneration(),
                        11, command.targetNodeRid(),
                        command.targetNodeGeneration(),
                        command.lastAcceptedSessionSequence()))
                .toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ((ZLinkFrameworkException) conflict.getCause()).kind());

        stream.pendingUnbinds.get("actor-1").complete(null);
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, first.toCompletableFuture().join().result());
        CompletableFuture.allOf(
            firstHeld.toCompletableFuture(),
            secondHeld.toCompletableFuture()).join();
        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(List.of("actor-1:held-1", "actor-1:held-2"),
            stream.relays.subList(
                stream.relays.size() - 2, stream.relays.size()));
    }

    @Test
    void failedNativePreparationKeepsTheSealForAnExactRetry() {
        FakeStream stream = new FakeStream();
        stream.relocationFailuresRemaining = 1;
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(
                seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> held = relay(actor, "held-after-failure");
        var command = route(relocation, 0);

        assertThrows(CompletionException.class,
            () -> runtime.applyRelocationRouteCommand(command)
                .toCompletableFuture().join());
        assertEquals(NODE_A, actor.ref().nodeRid());
        assertFalse(held.toCompletableFuture().isDone(),
            "a failed native preparation must not consume the seal");

        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, runtime.applyRelocationRouteCommand(command)
                .toCompletableFuture().join().result());
        held.toCompletableFuture().join();
        assertEquals(NODE_B, actor.ref().nodeRid());
    }

    //  Spec 20 §5 step 1/step 7: the seal fixes where the Session owner's
    //  accepted sequence stood, and command 43 reports that number back so the
    //  target can replay it in command 44.
    @Test
    void command42ReportsTheOwnersAcceptedSequenceAndIsIdempotent() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        relay(actor, 23);
        var seal = seal(relocation(), 7, NODE_A, 9);

        var sealed = runtime.applyRelocationSealCommand(seal)
            .toCompletableFuture().join();

        assertEquals(23, sealed.lastAcceptedSessionSequence());
        assertTrue(sealed.echoes(seal));

        //  A retransmitted byte-identical command 42 must answer from the
        //  cached terminal instead of re-reading the counter.
        CompletionStage<Void> held = relay(actor);
        assertFalse(held.toCompletableFuture().isDone());
        assertEquals(sealed, runtime.applyRelocationSealCommand(seal)
            .toCompletableFuture().join());
        runtime.applyRelocationRouteCommand(route(relocation(), 23))
            .toCompletableFuture().join();
        held.toCompletableFuture().join();
        assertEquals(24, stream.relays.size());
    }

    @Test
    void command42UsesTheSequenceAcceptedByThePhysicalSessionGate() {
        FakeStream stream = new FakeStream();
        stream.nextIngressSequence = 73;
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        relay(actor).toCompletableFuture().join();
        var sealed = runtime.applyRelocationSealCommand(
                seal(relocation(), 7, NODE_A, 9))
            .toCompletableFuture().join();

        assertEquals(73, sealed.lastAcceptedSessionSequence());
        assertEquals(List.of(73L), stream.relaySequences);
    }

    @Test
    void command42InstallsTheBarrierBeforeActiveIngressDrains() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        stream.blockRelayEntered = new CountDownLatch(1);
        stream.blockRelayRelease = new CountDownLatch(1);
        CompletableFuture<CompletionStage<Void>> activeStart =
            CompletableFuture.supplyAsync(() -> relay(actor, "active"));
        assertTrue(await(stream.blockRelayEntered),
            "pre-seal ingress did not enter the owner gate");
        var command = seal(relocation(), 7, NODE_A, 9);

        CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            first = runtime.applyRelocationSealCommand(command);
        CompletionStage<ZLinkServiceM6BWireCodec.SessionRelocationSealed>
            duplicate = runtime.applyRelocationSealCommand(command);
        CompletionStage<Void> held = relay(actor, "post-seal");

        assertSame(first, duplicate,
            "an identical command 42 shares the pending drain future");
        assertFalse(first.toCompletableFuture().isDone());
        assertFalse(held.toCompletableFuture().isDone());
        CompletionException conflict = assertThrows(CompletionException.class,
            () -> runtime.applyRelocationSealCommand(
                    seal(relocation(), 7, NODE_A, 11))
                .toCompletableFuture().join());
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ((ZLinkFrameworkException) conflict.getCause()).kind());

        stream.blockRelayRelease.countDown();
        activeStart.join().toCompletableFuture().join();
        assertEquals(1,
            first.toCompletableFuture().join()
                .lastAcceptedSessionSequence());
        assertFalse(held.toCompletableFuture().isDone(),
            "draining pre-seal ingress does not release post-seal ingress");

        runtime.applyRelocationRouteCommand(abort(relocation()))
            .toCompletableFuture().join();
        held.toCompletableFuture().join();
        assertEquals("actor-1:post-seal",
            stream.relays.get(stream.relays.size() - 1));
    }

    @Test
    void aConflictingCommand42ForTheSameRelocationIsRefused() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        runtime.applyRelocationSealCommand(seal(relocation(), 7, NODE_A, 9))
            .toCompletableFuture().join();

        //  Same relocation id, different Actor authority fence.
        assertThrows(CompletionException.class,
            () -> runtime.applyRelocationSealCommand(
                    seal(relocation(), 7, NODE_A, 11))
                .toCompletableFuture().join());

        //  A seal whose fence does not match the current binding is refused
        //  outright, so nothing is recorded and the owner keeps its gate.
        assertThrows(CompletionException.class,
            () -> runtime.applyRelocationSealCommand(
                    seal(new ZLinkServiceM6BWireCodec
                        .RelocationIdentity(31, 32), 8, NODE_A, 9))
                .toCompletableFuture().join());
    }

    @Test
    void command44MustReplayTheSealedHighWaterExactly() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        relay(actor, 23);
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> held = relay(actor);
        assertFalse(held.toCompletableFuture().isDone());

        //  A high-water above the sealed one no longer passes: the monotonic
        //  gate is replaced by spec 20 §5 step 7 equality once a seal exists.
        //  The refusal is answered with `stale` so the target stops
        //  retransmitting (spec 20 §5).
        var refused = runtime.applyRelocationRouteCommand(
            route(relocation, 24)).toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .STALE, refused.result());
        assertEquals(NODE_A, actor.ref().nodeRid());
        assertFalse(held.toCompletableFuture().isDone());

        var ack = runtime.applyRelocationRouteCommand(
                route(relocation, 23)).toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec
            .SessionRelocationRouteAction.COMMIT, ack.action());
        assertEquals(23, ack.lastAcceptedSessionSequence());
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, ack.result());
        assertEquals(NODE_B, actor.ref().nodeRid());
        held.toCompletableFuture().join();
        assertEquals(24, stream.relays.size());

        //  The already-applied branch stays ahead of the gate: a retransmit
        //  after the seal was consumed must re-ACK (spec 20 §5: the owner
        //  answers a repeated request instead of dropping it).
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .ALREADY_APPLIED, runtime.applyRelocationRouteCommand(
                route(relocation, 23)).toCompletableFuture().join().result());
    }

    //  Without a completed command 42 handshake the owner has no durable
    //  token and must not construct a route from a guessed monotonic value.
    @Test
    void anUnsealedCommand44IsRejectedWithoutChangingTheRoute() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        var ack = runtime.applyRelocationRouteCommand(
                route(relocation(), 999)).toCompletableFuture().join();

        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .STALE, ack.result());
        assertEquals(0, ack.lastAcceptedSessionSequence(),
            "a rejected command 44 supplies no owner high-water evidence");
        assertEquals(NODE_A, actor.ref().nodeRid());

        var closed = runtime(new FakeStream())
            .applyRelocationRouteCommand(route(
                new ZLinkServiceM6BWireCodec.RelocationIdentity(12, 13),
                999))
            .toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .SESSION_OR_BINDING_CLOSED, closed.result());
        assertEquals(0, closed.lastAcceptedSessionSequence());
    }

    @Test
    void committedTargetLeaseFencesTheNextRelocationExactly() {
        FakeStream stream = new FakeStream();
        ZLinkInternalSpotNode spotNode = authoritySpotNode(Map.of(
            NODE_A, new ActorAuthority(3, 9, 4),
            NODE_B, new ActorAuthority(4, 10, 14),
            NODE_C, new ActorAuthority(5, 11, 15)));
        ZLinkSessionActorsRuntime runtime = new ZLinkSessionActorsRuntime(
            spotNode,
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW);
        ZLinkSessionActor actor = runtime.bind(
                new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        var first = new ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
        runtime.applyRelocationSealCommand(
                seal(first, 7, NODE_A, 3, 9, 4))
            .toCompletableFuture().join();
        runtime.applyRelocationRouteCommand(
                route(first, 0, 9, 10, NODE_B, 4))
            .toCompletableFuture().join();
        assertEquals(NODE_B, actor.ref().nodeRid());

        var second = new ZLinkServiceM6BWireCodec.RelocationIdentity(10, 11);
        runtime.applyRelocationSealCommand(
                seal(second, 7, NODE_B, 4, 10, 14))
            .toCompletableFuture().join();
        runtime.applyRelocationRouteCommand(
                route(second, 0, 10, 11, NODE_C, 5))
            .toCompletableFuture().join();

        assertEquals(NODE_C, actor.ref().nodeRid(),
            "the second command 42 accepted the first target's live lease");
    }

    @Test
    void relocationHoldHasNoLegacyCountOrSixteenMebibyteCap() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();

        byte[] payload = new byte[16 * 1024];
        List<CompletableFuture<Void>> held = new ArrayList<>();
        ZLinkSessionActorsRuntime.enterRelayDispatch(header("Held"));
        try {
            for (int index = 0; index < 1_025; index++) {
                held.add(actor.relay(ZLinkMessage.fromEncoded(
                        ZLinkEncodedPayload.from(payload), new RawSerializer()))
                    .toCompletableFuture());
            }
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }

        assertTrue(held.stream().noneMatch(CompletableFuture::isDone));
        assertTrue(stream.relays.isEmpty());
        runtime.applyRelocationRouteCommand(abort(relocation))
            .toCompletableFuture().join();
        CompletableFuture.allOf(held.toArray(CompletableFuture[]::new)).join();
        assertEquals(1_025, stream.relays.size());
    }

    private static ZLinkServiceM6BWireCodec.RelocationIdentity relocation() {
        return new ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long objectGeneration,
        RoutingId actorNodeRid,
        long authorityOwnerGeneration) {
        return seal(
            relocation,
            objectGeneration,
            actorNodeRid,
            3,
            authorityOwnerGeneration,
            4);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long objectGeneration,
        RoutingId actorNodeRid,
        long actorNodeGeneration,
        long authorityOwnerGeneration,
        long authorityOwnerLeaseGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(
                    actorNodeRid, "actor-1", objectGeneration),
                actorNodeGeneration,
                authorityOwnerGeneration,
                authorityOwnerLeaseGeneration),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, BINDING_GENERATION));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long highWater) {
        return route(relocation, highWater, 9, 10, NODE_B, 4);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long highWater,
        long previousAuthorityOwnerGeneration,
        long currentAuthorityOwnerGeneration,
        RoutingId targetNodeRid,
        long targetNodeGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, BINDING_GENERATION),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            previousAuthorityOwnerGeneration,
            currentAuthorityOwnerGeneration,
            targetNodeRid,
            targetNodeGeneration,
            highWater);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute abort(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, BINDING_GENERATION),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            0, 9, null, 0, 0);
    }

    @Test
    void command44AbortAcksTerminallyWithoutChangingTheRoute() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        relay(actor, 3);
        runtime.applyRelocationSealCommand(
                seal(relocation(), 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> firstHeld = relay(actor, "abort-held-1");
        CompletionStage<Void> secondHeld = relay(actor, "abort-held-2");
        assertFalse(firstHeld.toCompletableFuture().isDone());
        assertFalse(secondHeld.toCompletableFuture().isDone());
        var abort = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                        "coordinator", 2, NODE_A, 3, "store-v4"),
                ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        NODE_A, 3, "session-owner", 4, SESSION, BINDING_GENERATION),
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.ABORT,
                0, 9, null, 0, 0);

        //  Internals 12 §"Ready 시점" and §"Session route": a pre-owner-change
        //  abort never moved the Session route, and spec 20 §6 forbids rolling
        //  a committed route back to the source. The abort therefore changes
        //  nothing, but it still has to be answered so the sender stops
        //  retransmitting (spec 20 §5 step 8).
        var ack = runtime.applyRelocationRouteCommand(abort)
            .toCompletableFuture().join();
        CompletableFuture.allOf(
            firstHeld.toCompletableFuture(),
            secondHeld.toCompletableFuture()).join();

        assertEquals(NODE_A, actor.ref().nodeRid());
        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            ack.action());
        assertEquals(9, ack.currentAuthorityOwnerGeneration());
        assertEquals(3, ack.lastAcceptedSessionSequence(),
            "abort command 45 echoes the matching seal terminal high-water");
        assertEquals(abort.relocation(), ack.relocation());
        assertEquals(abort.session(), ack.session());
        assertEquals(List.of("actor-1:abort-held-1", "actor-1:abort-held-2"),
            stream.relays.subList(stream.relays.size() - 2,
                stream.relays.size()));
        var replay = runtime.applyRelocationRouteCommand(abort)
            .toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .ALREADY_APPLIED, replay.result());
        assertEquals(3, replay.lastAcceptedSessionSequence());
    }

    private static CompletionStage<Void> relay(ZLinkSessionActor actor) {
        return relay(actor, "Play");
    }

    private static CompletionStage<Void> relay(
        ZLinkSessionActor actor,
        String packetName) {
        ZLinkSessionActorsRuntime.enterRelayDispatch(header(packetName));
        try {
            return actor.relay(ZLinkMessage.empty());
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }
    }

    private static void relay(ZLinkSessionActor actor, int count) {
        for (int index = 0; index < count; index++) {
            relay(actor).toCompletableFuture().join();
        }
    }

    private static ZLinkSessionActorsRuntime runtime(FakeStream stream) {
        return new ZLinkSessionActorsRuntime(
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW);
    }

    private static ZLinkInternalSpotNode authoritySpotNode(
        Map<RoutingId, ActorAuthority> authorities) {
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkSessionActorBindingContractTest.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> NODE_A;
                case "localNodeGeneration" -> 3L;
                case "localAuthorityOwnerId" -> "session-owner";
                case "localAuthorityLeaseGeneration" -> 4L;
                case "actorNodeGeneration" -> authorities.get(
                    ((ZLinkBackendActorRef) arguments[0]).nodeRid())
                    .nodeGeneration();
                case "actorAuthorityOwnerGeneration" -> authorities.get(
                    ((ZLinkBackendActorRef) arguments[0]).nodeRid())
                    .authorityOwnerGeneration();
                case "actorAuthorityOwnerLeaseGeneration" -> authorities.get(
                    ((ZLinkBackendActorRef) arguments[0]).nodeRid())
                    .ownerLeaseGeneration();
                case "rememberActorAuthority" -> throw new AssertionError(
                    "route commit must not overwrite the live target lease");
                case "name" -> "authority-spot";
                case "close" -> null;
                case "toString" -> "authority-spot";
                default -> throw new UnsupportedOperationException(
                    method.getName());
            });
    }

    private record ActorAuthority(
        long nodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
    }

    private static ZLinkStreamHeader header(String name) {
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            name,
            Map.of());
    }

    private static final class RawSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                String.valueOf(value).getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            throw new UnsupportedOperationException();
        }
    }

    //  The two unbind submits are dispatched as independent pool tasks, so
    //  they arrive in an arbitrary order and only their set is deterministic.
    private static void awaitUnbinds(FakeStream stream, String... expected) {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (stream.unbinds.size() < expected.length) {
            if (System.nanoTime() > deadline) {
                assertEquals(Set.of(expected), Set.copyOf(stream.unbinds),
                    "unbind submissions did not arrive");
                return;
            }
            Thread.onSpinWait();
        }
    }

    private static boolean await(CountDownLatch latch) {
        try {
            return latch.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError(interrupted);
        }
    }

    private static final class FakeStream implements ZLinkBackendStreamSocket {
        private final List<String> binds = new ArrayList<>();
        //  `unbindActor` is submitted from a pool thread (ZLinkBoundActor
        //  hops off the completing thread on purpose), so the recording list
        //  is read by the test thread while a pool thread appends to it.
        private final List<String> unbinds =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        private final List<String> relays = new ArrayList<>();
        private final List<Long> relaySequences = new ArrayList<>();
        private final Map<String, CompletableFuture<Void>> pendingUnbinds =
            new ConcurrentHashMap<>();
        private boolean deferUnbind;
        private SubmitResult relayFailure;
        private CompletableFuture<List<Message>> pendingBoundRequest;
        private CompletableFuture<List<Message>> pendingDisconnectNotification;
        private CountDownLatch blockRelayEntered;
        private CountDownLatch blockRelayRelease;
        private int relocationFailuresRemaining;
        private int disconnectNotifications;
        private long nextIngressSequence = 1;
        private boolean closed;
        @Override public String name() { return "session-contract"; }
        @Override public void close() { closed = true; }
        @Override public void bind(String endpoint) { }
        @Override public void setTlsServer(String certificatePath, String keyPath,
                                           boolean requireClientCertificate) { }
        @Override public void setMaxMessageSize(long value) { }
        @Override public void enableNotifications() { }
        @Override public boolean waitForReadable(Duration timeout) { return false; }
        @Override public ZLinkBackendStreamReceived recv() { return null; }
        @Override public void onTransportError(ZLinkBackendStreamErrorHandler handler) { }
        @Override public void startSessionService() { }
        @Override public long boundActorBindingGeneration(
            RoutingId sessionRid,
            String actorId) {
            return BINDING_GENERATION;
        }
        @Override public long allocateBoundSessionIngressSequence() {
            return nextIngressSequence++;
        }
        @Override public boolean send(
            RoutingId routingId, List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean send(
            RoutingId routingId, String packetName, List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean send(
            RoutingId routingId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean reply(
            RoutingId routingId, long requestSeq, String packetName,
            List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean reply(
            RoutingId routingId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public ZLinkBackendActorBindOperation bindActor(
            RoutingId sessionRid, ZLinkBackendActorRef actor) {
            binds.add(actor.actorId());
            return timeout -> CompletableFuture.completedFuture(null);
        }
        @Override public ZLinkBackendActorUnbindOperation unbindActor(
            RoutingId sessionRid, String actorId) {
            return timeout -> {
                if (deferUnbind) {
                    CompletableFuture<Void> pending = new CompletableFuture<>();
                    //  Publish the deferred future before the recording entry:
                    //  the test waits on `unbinds` and then resolves the
                    //  matching pending future.
                    pendingUnbinds.put(actorId, pending);
                    unbinds.add(actorId);
                    return pending;
                }
                unbinds.add(actorId);
                return CompletableFuture.completedFuture(null);
            };
        }
        @Override public CompletionStage<Void> relocateBoundActor(
            RoutingId sessionRid,
            String actorId,
            long bindingGeneration,
            ZLinkBackendActorRef targetActor,
            Duration timeout) {
            if (relocationFailuresRemaining > 0) {
                relocationFailuresRemaining--;
                return CompletableFuture.failedFuture(
                    new IllegalStateException("forced native route failure"));
            }
            return ZLinkBackendStreamSocket.super.relocateBoundActor(
                sessionRid,
                actorId,
                bindingGeneration,
                targetActor,
                timeout);
        }
        @Override public boolean sendBoundActor(
            RoutingId sessionRid, String actorId,
            List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean relayBoundActor(
            RoutingId sessionRid, String actorId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) {
            relays.add(actorId + ":" + header.packetName());
            if (blockRelayEntered != null) {
                blockRelayEntered.countDown();
                CountDownLatch release = blockRelayRelease;
                blockRelayEntered = null;
                if (!await(release)) {
                    throw new AssertionError("blocked relay was not released");
                }
            }
            if (relayFailure != null) {
                throw new ZlinkSubmitException(relayFailure);
            }
            return true;
        }
        @Override public boolean relayBoundActor(
            RoutingId sessionRid,
            String actorId,
            long sourceSessionSequence,
            ZLinkStreamHeader header,
            List<Message> parts,
            SendFlags flags) {
            relaySequences.add(sourceSessionSequence);
            return relayBoundActor(
                sessionRid, actorId, header, parts, flags);
        }
        @Override public CompletionStage<List<Message>>
            requestBoundActor(
                RoutingId sessionRid,
                String actorId,
                ZLinkStreamHeader header,
                List<Message> parts,
                Duration timeout) {
            if (ZLinkActorSpotRoutePackets.SESSION_DISCONNECTED_PACKET_NAME.equals(
                    header.packetName())) {
                disconnectNotifications++;
                if (pendingDisconnectNotification != null) {
                    CompletableFuture<List<Message>> pending =
                        pendingDisconnectNotification;
                    pendingDisconnectNotification = null;
                    return pending;
                }
            } else if (pendingBoundRequest != null) {
                return pendingBoundRequest;
            }
            return CompletableFuture.completedFuture(
                List.of(Message.from(new byte[0])));
        }
        @Override public CompletionStage<List<Message>>
            requestExactActor(
                ZLinkBackendActorRef actor,
                ZLinkStreamHeader header,
                List<Message> parts,
                Duration timeout) {
            if (ZLinkActorSpotRoutePackets.SESSION_DISCONNECTED_PACKET_NAME.equals(
                    header.packetName())) {
                disconnectNotifications++;
                if (pendingDisconnectNotification != null) {
                    CompletableFuture<List<Message>> pending =
                        pendingDisconnectNotification;
                    pendingDisconnectNotification = null;
                    return pending;
                }
            }
            return CompletableFuture.completedFuture(
                List.of(Message.from(new byte[0])));
        }
    }
}
