package systems.zlink.framework.runtime.actors;
import java.util.concurrent.ConcurrentHashMap;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
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
import java.util.concurrent.atomic.AtomicInteger;
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
    void synchronousDisconnectSubmissionFailureStillUnbindsEveryActor() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor first = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A)).toCompletableFuture().join();
        runtime.bind(new ActorRef("actor-2", 8, MESH, NODE_B))
            .toCompletableFuture().join();
        var rejected = new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
        stream.disconnectSubmissionFailure = rejected;

        CompletionException failure = assertThrows(CompletionException.class,
            () -> runtime.notifyDisconnectedAll().toCompletableFuture().join());

        assertSame(rejected, failure.getCause());
        assertEquals(2, stream.disconnectNotifications);
        assertEquals(Set.of("actor-1", "actor-2"), Set.copyOf(stream.unbinds));
        assertEquals(2, stream.unbinds.size());
        assertTrue(runtime.bound().isEmpty());
        assertTrue(first.notifyDisconnected().toCompletableFuture()
            .isCompletedExceptionally());
        assertEquals(2, stream.disconnectNotifications);
        assertEquals(2, stream.unbinds.size());
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
    void actorIngressIgnoresStaleGateProjectionForCurrentBinding()
        throws Exception {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        corruptIngressGateProjection(runtime, 70, BINDING_GENERATION + 1);

        relay(actor, "projection-mismatch").toCompletableFuture().join();

        assertEquals("actor-1:projection-mismatch", stream.relays.getLast());
    }

    @Test
    void boundSessionSendUsesOnlySourceThreeAndExpectedBinding() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(
            stream,
            authoritySpotNode(Map.of(
                NODE_A, new ActorAuthority(3, 9, 4))));
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var payload = outboundPayload("projection-independent");

        assertTrue(runtime.acceptBoundSessionSend(
            NODE_A, 3, boundSend(NODE_A, 3, 9, 400), payload));
        assertEquals(List.of("projection-independent"), stream.boundPushes);
    }

    @Test
    void asyncBoundSessionSendSubmitsInSessionFifoWithoutWaiting() {
        FakeStream stream = new FakeStream();
        stream.deferBoundPushAdmission = true;
        ZLinkSessionActorsRuntime runtime = runtime(
            stream,
            authoritySpotNode(Map.of(
                NODE_A, new ActorAuthority(3, 9, 4))));
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();

        CompletionStage<Boolean> first = runtime.acceptBoundSessionSendAsync(
            NODE_A,
            3,
            boundSend(NODE_A, 3, 9, 400),
            outboundPayload("first"));
        CompletionStage<Boolean> second = runtime.acceptBoundSessionSendAsync(
            NODE_A,
            3,
            boundSend(NODE_A, 3, 9, 400),
            outboundPayload("second"));

        awaitBoundPushAdmissions(stream, 2);
        assertEquals(List.of("first", "second"), stream.boundPushes);
        assertFalse(first.toCompletableFuture().isDone());
        assertFalse(second.toCompletableFuture().isDone());

        stream.boundPushAdmissions.getFirst().complete(null);
        assertTrue(first.toCompletableFuture().join());
        stream.boundPushAdmissions.getLast().complete(null);
        assertTrue(second.toCompletableFuture().join());
    }

    @Test
    void command42DoesNotRejudgeActorOrSessionAuthorityMirrors() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(
            stream,
            authoritySpotNode(Map.of(
                NODE_A, new ActorAuthority(30, 90, 40))));
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var command = seal(relocation(), 7, NODE_A, 9);

        var reply = runtime.applyRelocationSealCommand(command)
            .toCompletableFuture().join();

        assertTrue(reply.echoes(command));
    }


    @Test
    void command42EchoesTheExactFenceAndCommand44IsOneWay() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        var command42 = seal(relocation, 7, NODE_A, 9);

        var command43 = runtime.applyRelocationSealCommand(command42)
            .toCompletableFuture().join();
        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();

        assertTrue(command43.echoes(command42));
        assertEquals(NODE_B, actor.ref().nodeRid());
        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();
        assertEquals(NODE_B, actor.ref().nodeRid(),
            "duplicate one-way command 44 is idempotent");
    }

    @Test
    void exactSourceAbortReleasesHeldIngressWithoutReply() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> held = relay(actor, "held");

        runtime.applyRelocationRouteCommand(abort(relocation))
            .toCompletableFuture().join();
        held.toCompletableFuture().join();

        assertEquals(NODE_A, actor.ref().nodeRid());
        assertEquals("actor-1:held", stream.relays.getLast());
        runtime.applyRelocationRouteCommand(abort(relocation))
            .toCompletableFuture().join();
    }

    @Test
    void command44CommitDoesNotRejudgeSourceAuthorityMirror() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();

        runtime.applyRelocationRouteCommand(route(relocation, 8, 10))
            .toCompletableFuture().join();

        assertEquals(NODE_B, actor.ref().nodeRid());
    }

    @Test
    void command44AbortUsesMatchingSealIdentityNotAuthorityMirror() {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(stream);
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> held = relay(actor, "held-authority-mismatch");

        runtime.applyRelocationRouteCommand(abort(relocation, 1))
            .toCompletableFuture().join();

        held.toCompletableFuture().join();
        assertEquals("actor-1:held-authority-mismatch", stream.relays.getLast());
    }

    @Test
    void sealDeadlineClosesThePhysicalSessionAndIgnoresLateRouteUpdate()
        throws Exception {
        FakeStream stream = new FakeStream();
        // The expiry itself is invoked directly below, so this injected value
        // keeps the test deterministic without waiting for wall-clock time.
        ZLinkSessionActorsRuntime runtime = runtime(
            stream, Duration.ofMinutes(1));
        ZLinkSessionActor actor = runtime.bind(
            new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture().join();
        var relocation = relocation();
        runtime.applyRelocationSealCommand(seal(relocation, 7, NODE_A, 9))
            .toCompletableFuture().join();
        CompletionStage<Void> held = relay(actor, "held-after-seal");

        expireRelocationSeal(runtime);

        assertEquals(1, stream.disconnectedPeers);
        assertTrue(runtime.bound().isEmpty());
        assertThrows(CompletionException.class,
            () -> held.toCompletableFuture().join());
        runtime.applyRelocationRouteCommand(route(relocation))
            .toCompletableFuture().join();
        assertEquals(1, stream.disconnectedPeers,
            "a late command 44 cannot reactivate or close the Session twice");
    }

    @Test
    void configuredSealDeadlineArmsTheActualSessionScheduler()
        throws Exception {
        FakeStream stream = new FakeStream();
        ZLinkSessionActorsRuntime runtime = runtime(
            stream, Duration.ofMillis(17));
        runtime.bind(new ActorRef("actor-1", 7, MESH, NODE_A))
            .toCompletableFuture()
            .join();

        runtime.applyRelocationSealCommand(
                seal(relocation(), 7, NODE_A, 9))
            .toCompletableFuture()
            .join();

        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!runtime.bound().isEmpty() && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertTrue(runtime.bound().isEmpty(),
            "the configured Session seal deadline did not expire");
        assertEquals(1, stream.disconnectedPeers);
    }

    private static void expireRelocationSeal(ZLinkSessionActorsRuntime runtime)
        throws Exception {
        Field terminals = ZLinkSessionActorsRuntime.class
            .getDeclaredField("sealTerminals");
        terminals.setAccessible(true);
        Object terminal = ((Map<?, ?>) terminals.get(runtime)).values()
            .iterator().next();
        Method expiration = java.util.Arrays.stream(
                ZLinkSessionActorsRuntime.class.getDeclaredMethods())
            .filter(method -> method.getName().equals("expireRelocationSeal"))
            .findFirst()
            .orElseThrow();
        expiration.setAccessible(true);
        expiration.invoke(runtime, terminal);
    }

    private static ZLinkServiceM6BWireCodec.RelocationIdentity relocation() {
        return new ZLinkServiceM6BWireCodec.RelocationIdentity(8, 9);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationSeal seal(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long actorGeneration,
        RoutingId actorNodeRid,
        long authorityOwnerGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(
                    actorNodeRid, "actor-1", actorGeneration),
                3, authorityOwnerGeneration, 4),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION,
                BINDING_GENERATION));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation) {
        return route(relocation, 9, 10);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute route(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long previousAuthorityOwnerGeneration,
        long currentAuthorityOwnerGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.TARGET,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION,
                BINDING_GENERATION),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.COMMIT,
            previousAuthorityOwnerGeneration,
            currentAuthorityOwnerGeneration,
            NODE_B,
            4);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute abort(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation) {
        return abort(relocation, 9);
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute abort(
        ZLinkServiceM6BWireCodec.RelocationIdentity relocation,
        long currentAuthorityOwnerGeneration) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "coordinator", 2, NODE_A, 3, "store-v4"),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorIdentity("actor-1", 7),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                NODE_A, 3, "session-owner", 4, SESSION,
                BINDING_GENERATION),
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            0, currentAuthorityOwnerGeneration, null, 0);
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

    private static ZLinkSessionActorsRuntime runtime(
        FakeStream stream,
        ZLinkInternalSpotNode spotNode) {
        return new ZLinkSessionActorsRuntime(
            spotNode,
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW);
    }

    private static void corruptIngressGateProjection(
        ZLinkSessionActorsRuntime runtime,
        long objectGeneration,
        long bindingGeneration) throws Exception {
        Field gatesField = ZLinkSessionActorsRuntime.class
            .getDeclaredField("ingressGates");
        gatesField.setAccessible(true);
        Object gate = ((Map<?, ?>) gatesField.get(runtime)).get("actor-1");
        Field objectField = gate.getClass().getDeclaredField("objectGeneration");
        objectField.setAccessible(true);
        objectField.setLong(gate, objectGeneration);
        Field bindingField = gate.getClass().getDeclaredField("bindingGeneration");
        bindingField.setAccessible(true);
        bindingField.setLong(gate, bindingGeneration);
    }

    private static ZLinkSessionActorsRuntime runtime(
        FakeStream stream,
        Duration sessionRelocationSealTimeout) {
        return new ZLinkSessionActorsRuntime(
            null,
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW,
            null,
            sessionRelocationSealTimeout);
    }

    private static ZLinkSessionActorsRuntime runtime(
        FakeStream stream,
        int targetOutboundCapacity) {
        return new ZLinkSessionActorsRuntime(
            null,
            stream,
            SESSION,
            null,
            new RawSerializer(),
            ignored -> true,
            null,
            true,
            ZLinkStreamCodec.RAW,
            null,
            targetOutboundCapacity);
    }

    private static ZLinkServiceM6BWireCodec.BoundSessionSend boundSend(
        RoutingId nodeRid,
        long nodeGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
        return new ZLinkServiceM6BWireCodec.BoundSessionSend(
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(nodeRid, "actor-1", 7),
                nodeGeneration,
                authorityOwnerGeneration,
                ownerLeaseGeneration),
            BINDING_GENERATION);
    }

    private static systems.zlink.framework.runtime.internal.service
        .ZLinkServiceM6AWireCodec.ApplicationPayload outboundPayload(
            String value) {
        try (Message message = Message.from(
                value.getBytes(StandardCharsets.UTF_8))) {
            return systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6AWireCodec.encodeFrameworkMultipart(
                    List.of(message));
        }
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

    private static void awaitBoundPushes(FakeStream stream, int expected) {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (stream.boundPushes.size() < expected) {
            if (System.nanoTime() > deadline) {
                assertEquals(expected, stream.boundPushes.size(),
                    "bound Session FIFO did not drain");
                return;
            }
            Thread.onSpinWait();
        }
    }

    private static void awaitBoundPushAdmissions(
        FakeStream stream,
        int expected) {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (stream.boundPushAdmissions.size() < expected) {
            if (System.nanoTime() > deadline) {
                assertEquals(expected, stream.boundPushAdmissions.size(),
                    "bound Session physical admission did not start");
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
        private Runnable relocationEntered;
        private volatile boolean boundPushAvailable = true;
        private final List<String> boundPushes =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        private final List<CompletableFuture<Void>> boundPushAdmissions =
            new java.util.concurrent.CopyOnWriteArrayList<>();
        private boolean deferBoundPushAdmission;
        private int disconnectNotifications;
        private RuntimeException disconnectSubmissionFailure;
        private int disconnectedPeers;
        private long nextIngressSequence = 1;
        private boolean closed;
        @Override public String name() { return "session-contract"; }
        @Override public void close() { closed = true; }
        @Override public void bind(String endpoint) { }
        @Override public void setTlsServer(String certificatePath, String keyPath,
                                           boolean requireClientCertificate) { }
        @Override public void setMaxMessageSize(long value) { }
        @Override public void disconnectPeer(RoutingId routingId) {
            disconnectedPeers++;
        }
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
            if (relocationEntered != null) {
                relocationEntered.run();
            }
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
        @Override public boolean sendBoundSessionPush(
            RoutingId sessionRid,
            List<Message> parts,
            SendFlags flags) {
            if (!boundPushAvailable) {
                return false;
            }
            boundPushes.add(new String(
                parts.getFirst().toByteArray(), StandardCharsets.UTF_8));
            return true;
        }
        @Override public CompletionStage<Void> sendBoundSessionPushAsync(
            RoutingId sessionRid,
            List<Message> parts) {
            if (!deferBoundPushAdmission) {
                return ZLinkBackendStreamSocket.super
                    .sendBoundSessionPushAsync(sessionRid, parts);
            }
            boundPushes.add(new String(
                parts.getFirst().toByteArray(), StandardCharsets.UTF_8));
            CompletableFuture<Void> admission = new CompletableFuture<>();
            boundPushAdmissions.add(admission);
            return admission;
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
                if (disconnectSubmissionFailure != null) {
                    RuntimeException failure = disconnectSubmissionFailure;
                    disconnectSubmissionFailure = null;
                    throw failure;
                }
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
