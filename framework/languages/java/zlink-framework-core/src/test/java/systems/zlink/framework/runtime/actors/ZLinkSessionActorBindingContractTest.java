package systems.zlink.framework.runtime.actors;

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
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
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
        assertEquals(NODE_B, replacement.ref().nodeRid());
        assertEquals(7, replacement.ref().objectGeneration());

        ZLinkSessionActor newIncarnation = runtime.bind(
            new ActorRef("actor-1", 8, MESH, NODE_A)).toCompletableFuture().join();
        replacement.notifyDisconnected().toCompletableFuture().join();
        assertTrue(stream.unbinds.isEmpty());
        assertEquals(8, newIncarnation.ref().objectGeneration());
        assertEquals(List.of(newIncarnation), runtime.bound());
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
        assertEquals(List.of("actor-a", "actor-b"), stream.unbinds);
        stream.pendingUnbinds.get("actor-a").completeExceptionally(
            new IllegalStateException("forced unbind failure"));
        stream.pendingUnbinds.get("actor-b").complete(null);

        assertThrows(
            CompletionException.class,
            () -> first.toCompletableFuture().join());
        assertThrows(
            CompletionException.class,
            () -> duplicate.toCompletableFuture().join());
        assertEquals(List.of("actor-a", "actor-b"), stream.unbinds);
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
                systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                    .RelocationRole.TARGET,
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
                session,
                systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                    .SessionRelocationRouteAction.COMMIT,
                9, 10, NODE_B, 4, 17);

        var ack = runtime.applyRelocationRouteCommand(command)
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
        assertEquals(7, actor.ref().objectGeneration());
        assertEquals(10, ack.currentAuthorityOwnerGeneration());
        assertEquals(17, ack.lastAcceptedSessionSequence());
        assertThrows(CompletionException.class,
            () -> runtime.applyRelocationRouteCommand(command)
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
            EnumSet.noneOf(systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag.class),
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

    private static final class FakeStream implements ZLinkBackendStreamSocket {
        private final List<String> binds = new ArrayList<>();
        private final List<String> unbinds = new ArrayList<>();
        private final List<String> relays = new ArrayList<>();
        private final Map<String, CompletableFuture<Void>> pendingUnbinds =
            new java.util.concurrent.ConcurrentHashMap<>();
        private boolean deferUnbind;
        private SubmitResult relayFailure;
        private CompletableFuture<List<Message>> pendingBoundRequest;
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
                unbinds.add(actorId);
                if (deferUnbind) {
                    CompletableFuture<Void> pending = new CompletableFuture<>();
                    pendingUnbinds.put(actorId, pending);
                    return pending;
                }
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
        @Override public java.util.concurrent.CompletionStage<List<Message>>
            requestBoundActor(
                RoutingId sessionRid,
                String actorId,
                ZLinkStreamHeader header,
                List<Message> parts,
                Duration timeout) {
            if (pendingBoundRequest != null) {
                return pendingBoundRequest;
            }
            return CompletableFuture.completedFuture(
                List.of(Message.from(new byte[0])));
        }
    }
}
