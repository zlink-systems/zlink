package systems.zlink.framework.runtime.actors;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

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
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkSessionActorBindingContractTest {
    private static final RoutingId SESSION = RoutingId.from("session-a");
    private static final RoutingId NODE_A = RoutingId.from("actor-node-a");
    private static final RoutingId NODE_B = RoutingId.from("actor-node-b");
    private static final String MESH = "game";

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
        runtime.applyRelocationRouteUpdate(
            new ZLinkSessionActorsRuntime.RelocationRouteUpdate(
                "actor-1", 7, NODE_B, NODE_A, 4, 9, 10))
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

        runtime.applyRelocationRouteUpdate(
            new ZLinkSessionActorsRuntime.RelocationRouteUpdate(
                "actor-1", 7, NODE_A, NODE_B, 4, 9, 10))
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(7, actor.ref().objectGeneration());
        assertThrows(
            CompletionException.class,
            () -> runtime.applyRelocationRouteUpdate(
                    new ZLinkSessionActorsRuntime.RelocationRouteUpdate(
                        "actor-1", 7, NODE_A, NODE_B, 5, 9, 10))
                .toCompletableFuture().join(),
            "a repeated command with the stale source route must not ACK");
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
                NODE_A, 3, "session-owner", 4, SESSION, 1);
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
                9, 10, NODE_B, 4, 17);

        var ack = runtime.applyRelocationRouteCommand(command)
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(7, actor.ref().objectGeneration());
        assertEquals(10, ack.currentAuthorityOwnerGeneration());
        assertEquals(17, ack.lastAcceptedSessionSequence());
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, ack.result());
        //  Spec 20 §5: the session owner must answer a retransmitted command
        //  44 instead of failing the source-fence CAS, or a lost command 45
        //  leaves the target retrying forever. The echoed fence is identical;
        //  only the result names the repeat.
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
    }

    //  Spec 20 §5 step 1/step 7: the seal fixes where the Session owner's
    //  accepted sequence stood, and command 43 reports that number back so the
    //  target can replay it in command 44.
    @Test
    void command42ReportsTheOwnersAcceptedSequenceAndIsIdempotent() {
        FakeStream stream = new FakeStream();
        stream.boundSessionHighWater = 23;
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var seal = seal(relocation(), 7, NODE_A, 9);

        var sealed = runtime.applyRelocationSealCommand(seal)
            .toCompletableFuture().join();

        assertEquals(23, sealed.lastAcceptedSessionSequence());
        assertTrue(sealed.echoes(seal));

        //  A retransmitted byte-identical command 42 must answer from the
        //  cached terminal instead of re-reading the counter.
        stream.boundSessionHighWater = 91;
        assertEquals(sealed, runtime.applyRelocationSealCommand(seal)
            .toCompletableFuture().join());
    }

    @Test
    void aConflictingCommand42ForTheSameRelocationIsRefused() {
        FakeStream stream = new FakeStream();
        stream.boundSessionHighWater = 5;
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
        stream.boundSessionHighWater = 23;
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();

        //  A high-water above the sealed one no longer passes: the monotonic
        //  gate is replaced by spec 20 §5 step 7 equality once a seal exists.
        //  The refusal is answered with `stale` so the target stops
        //  retransmitting (spec 20 §5).
        var refused = runtime.applyRelocationRouteCommand(
            route(relocation, 24)).toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .STALE, refused.result());
        assertEquals(NODE_A, actor.ref().nodeRid());

        var ack = runtime.applyRelocationRouteCommand(
                route(relocation, 23)).toCompletableFuture().join();
        assertEquals(ZLinkServiceM6BWireCodec
            .SessionRelocationRouteAction.COMMIT, ack.action());
        assertEquals(23, ack.lastAcceptedSessionSequence());
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .APPLIED, ack.result());
        assertEquals(NODE_B, actor.ref().nodeRid());

        //  The already-applied branch stays ahead of the gate: a retransmit
        //  after the seal was consumed must re-ACK (spec 20 §5: the owner
        //  answers a repeated request instead of dropping it).
        assertEquals(ZLinkServiceM6BWireCodec.SessionRelocationRouteResult
            .ALREADY_APPLIED, runtime.applyRelocationRouteCommand(
                route(relocation, 23)).toCompletableFuture().join().result());
    }

    //  Without a completed command 42 handshake the owner has no reference
    //  value, so the monotonic gate stays in force for that relocation.
    @Test
    void anUnsealedCommand44KeepsTheMonotonicGate() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        var ack = runtime.applyRelocationRouteCommand(
                route(relocation(), 17)).toCompletableFuture().join();

        assertEquals(ZLinkServiceM6BWireCodec
            .SessionRelocationRouteAction.COMMIT, ack.action());
        assertEquals(NODE_B, actor.ref().nodeRid());
    }

    private static ZLinkServiceM6BWireCodec.RelocationIdentity relocation() {
        return new ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long objectGeneration,
        RoutingId actorNodeRid,
        long authorityOwnerGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(
                    actorNodeRid, "actor-1", objectGeneration),
                3, authorityOwnerGeneration, 4),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, 1));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long highWater) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION, 1),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            9, 10, NODE_B, 4, highWater);
    }

    @Test
    void command44AbortAcksTerminallyWithoutChangingTheRoute() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var abort = new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                        "coordinator", 2, NODE_A, 3, "store-v4"),
                ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        NODE_A, 3, "session-owner", 4, SESSION, 1),
                ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.ABORT,
                0, 10, null, 0, 0);

        //  Internals 12 §"Ready 시점" and §"Session route": a pre-owner-change
        //  abort never moved the Session route, and spec 20 §6 forbids rolling
        //  a committed route back to the source. The abort therefore changes
        //  nothing, but it still has to be answered so the sender stops
        //  retransmitting (spec 20 §5 step 8).
        var ack = runtime.applyRelocationRouteCommand(abort)
            .toCompletableFuture().join();

        assertEquals(NODE_A, actor.ref().nodeRid());
        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            ack.action());
        assertEquals(10, ack.currentAuthorityOwnerGeneration());
        assertEquals(abort.relocation(), ack.relocation());
        assertEquals(abort.session(), ack.session());
        assertEquals(ack, runtime.applyRelocationRouteCommand(abort)
            .toCompletableFuture().join());
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

    private static final class FakeStream implements ZLinkBackendStreamSocket {
        private final List<String> binds = new ArrayList<>();
        //  `unbindActor` is submitted from a pool thread (ZLinkBoundActor
        //  hops off the completing thread on purpose), so the recording list
        //  is read by the test thread while a pool thread appends to it.
        private final List<String> unbinds =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        private final List<String> relays = new ArrayList<>();
        private final Map<String, CompletableFuture<Void>> pendingUnbinds =
            new ConcurrentHashMap<>();
        private boolean deferUnbind;
        private SubmitResult relayFailure;
        private CompletableFuture<List<Message>> pendingBoundRequest;
        private CompletableFuture<List<Message>> pendingDisconnectNotification;
        private int disconnectNotifications;
        private boolean closed;
        private long boundSessionHighWater;

        @Override public long boundSessionSequenceHighWater() {
            return boundSessionHighWater;
        }

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
        @Override public boolean sendBoundActor(
            RoutingId sessionRid, String actorId,
            List<Message> parts, SendFlags flags) {
            return true;
        }
        @Override public boolean relayBoundActor(
            RoutingId sessionRid, String actorId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) {
            relays.add(actorId + ":" + header.packetName());
            if (relayFailure != null) {
                throw new ZlinkSubmitException(relayFailure);
            }
            return true;
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
