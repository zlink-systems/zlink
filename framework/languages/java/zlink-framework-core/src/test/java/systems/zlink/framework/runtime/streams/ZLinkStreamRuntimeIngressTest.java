package systems.zlink.framework.runtime.streams;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.EnumSet;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamRuntimeIngressTest {
    private static final RoutingId PEER_A = RoutingId.from("peer-a");
    private static final RoutingId PEER_B = RoutingId.from("peer-b");
    private static final String MESH = "replacement-mesh";
    private final List<ZLinkStreamRuntime> runtimes = new ArrayList<>();
    private ZLinkFrameworkRegistration lastRegistration;

    @AfterEach
    void resetSessionProbe() {
        TestSession.holdFirstDispatch = false;
        TestSession.failNextConstruction = false;
        TestSession.replacementMode = ReplacementMode.NONE;
        TestSession.decodeWirePayload = false;
        TestSession.decodedWirePayload.set(null);
        TestSession.createdCount.set(0);
        TestSession.lastSession.set(null);
        runtimes.forEach(runtime -> runtime.closeAsync().toCompletableFuture().join());
        runtimes.clear();
        lastRegistration = null;
    }

    @Test
    void usesRecvModeAndPreservesTheSourceRoutingId() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, new byte[0]);
        byte[] frame = frame("segmented", "{}");
        stream.enqueue(PEER_A, Arrays.copyOfRange(frame, 0, 4));
        stream.enqueue(PEER_A, Arrays.copyOfRange(frame, 4, frame.length));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(PEER_A, session.context.routingId().orElseThrow());
        assertTrue(stream.notificationsEnabled);
        assertTrue(stream.boundAfterNotifications);
        assertTrue(stream.successfulReceives.get() >= 3);
        assertTrue(stream.readinessWaits.get() >= 3);
        assertEquals(List.of("segmented"), session.packetNames);
    }

    @Test
    void sessionPayloadDecodeUsesTheSerializerMappedByTheWireCodec() throws Exception {
        TestSession.decodeWirePayload = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(
            PEER_A,
            frame("custom", ZLinkStreamCodec.PROTOBUF, "wire"));

        ZLinkStreamRuntime runtime = startWithCustomReceiveCodec(stream);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(
            new WirePayload("CUSTOM"),
            TestSession.decodedWirePayload.get());
    }

    @Test
    void continuesReceivingAcrossSerializedSessionDispatch()
        throws Exception {
        TestSession.holdFirstDispatch = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("first", "a"));
        stream.enqueue(PEER_A, frame("second", "b"));

        ZLinkStreamRuntime runtime = start(stream, 1);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertTrue(session.dispatchCount.get() >= 1);
        assertFalse(session.firstDispatch.isDone());

        assertEquals(2, stream.successfulReceives.get());
        assertFalse(session.secondDispatchLatch.await(100, TimeUnit.MILLISECONDS));
        assertEquals(1, session.dispatchCount.get());

        session.firstDispatch.complete(null);
        assertTrue(session.secondDispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(2, session.dispatchCount.get());
        assertEquals(List.of("first", "second"), session.packetNames);
    }

    @Test
    void retainsOneRawReceiveThroughEveryHandlerTerminalAndThenProgresses()
        throws Exception {
        TestSession.holdFirstDispatch = true;
        byte[] first = frame("first-retained", "a");
        byte[] second = frame("second-retained", "b");
        byte[] combined = new byte[first.length + second.length];
        System.arraycopy(first, 0, combined, 0, first.length);
        System.arraycopy(second, 0, combined, first.length, second.length);
        AtomicInteger ownerCloses = new AtomicInteger();
        FakeStream stream = new FakeStream();
        stream.enqueueTracked(PEER_A, combined, ownerCloses);

        ZLinkStreamRuntime runtime = start(stream, 1);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertFalse(session.firstDispatch.isDone());
        assertEquals(0, ownerCloses.get());

        session.firstDispatch.complete(null);
        assertTrue(session.secondDispatchLatch.await(5, TimeUnit.SECONDS));
        awaitValue(ownerCloses, 1);
        assertEquals(
            List.of("first-retained", "second-retained"),
            session.packetNames);

        stream.enqueue(PEER_A, frame("after-release", "c"));
        awaitValue(session.dispatchCount, 3);
        assertEquals(
            List.of("first-retained", "second-retained", "after-release"),
            session.packetNames);
    }

    @Test
    void isolatesMalformedPeerAndContinuesReceivingAnotherPeer() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, ZLinkStreamFrameCodec.encode(new byte[0], new byte[0]));
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(List.of("good"), session.packetNames);
        assertTrue(stream.sessionClosingSends.get() >= 1);
    }

    @Test
    void isolatesPeerWithNoMessagePartsAndContinuesReceivingAnotherPeer()
        throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueueEmptyParts(PEER_A);
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(PEER_B, session.context.routingId().orElseThrow());
        assertEquals(List.of("good"), session.packetNames);
        assertTrue(stream.sessionClosingSends.get() >= 1);
    }

    @Test
    void segmentedOversizeRecordsEmsgsizeAndDisconnectsThePeer() throws Exception {
        FakeStream stream = new FakeStream();
        byte[] oversize = frame("oversize", "x".repeat(512));
        stream.enqueue(PEER_A, Arrays.copyOf(oversize, 6));
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0, 256);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(List.of("good"), session.packetNames);
        assertEquals(PEER_A, stream.disconnectedPeer.get());
    }

    @Test
    void heartbeatBackpressureDoesNotIsolateThePeer() throws Exception {
        FakeStream stream = new FakeStream();
        stream.failHeartbeatPongSend = true;
        stream.enqueue(PEER_A, controlFrame("$zlink.heartbeat.ping"));
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(1, TestSession.createdCount.get());
        assertEquals(List.of("good"), session.packetNames);
        assertEquals(0, stream.sessionClosingSends.get());
    }

    @Test
    void sessionConstructionFailureDoesNotStopAnotherPeer() throws Exception {
        TestSession.failNextConstruction = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, new byte[0]);
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(1, TestSession.createdCount.get());
        assertEquals(PEER_B, session.context.routingId().orElseThrow());
        assertEquals(List.of("good"), session.packetNames);
    }

    @Test
    void ignoredPeerNotificationDoesNotCreatePhantomSession() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, ZLinkStreamFrameCodec.encode(new byte[0], new byte[0]));
        stream.enqueue(PEER_A, new byte[0]);
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(1, TestSession.createdCount.get());
        assertEquals(PEER_B, session.context.routingId().orElseThrow());
        assertEquals(List.of("good"), session.packetNames);
    }

    @Test
    void drainingDoesNotChargeTheApplicationBudgetForRejectedFrames() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("rejected", "{}"));
        stream.blockFirstReceive();

        ZLinkStreamRuntime runtime = start(stream, 1);
        runtimes.add(runtime);

        assertTrue(stream.firstReceiveEntered.await(5, TimeUnit.SECONDS));
        runtime.beginDrain();
        stream.firstReceiveRelease.countDown();

        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (stream.sessionClosingSends.get() == 0
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertEquals(1, stream.sessionClosingSends.get());
        assertNull(TestSession.lastSession.get());
    }

    @Test
    void waitsForReceiveLoopQuiescenceBeforeClosingTheStream() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, new byte[0]);
        stream.blockFirstReceiveIgnoringInterrupt();
        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        assertTrue(stream.firstReceiveEntered.await(5, TimeUnit.SECONDS));
        CompletableFuture<Void> close = CompletableFuture.runAsync(
            () -> runtime.closeAsync().toCompletableFuture().join());
        Thread.sleep(2_200);
        assertEquals(0, stream.closeCalls.get());

        stream.firstReceiveRelease.countDown();
        close.get(5, TimeUnit.SECONDS);
        assertEquals(1, stream.closeCalls.get());
    }

    @Test
    void boundSessionReplacementRunsCallbackBeforeADeferredCloseAndRejectsInbound() throws Exception {
        TestSession.replacementMode = ReplacementMode.FAILURE;
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());

        TestSession session = awaitSession();
        ZLinkActor actor = fixture.actors().getOrCreateLocalActor(
                "replacement-actor", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context().actors().bind(new ActorRef(
                actorRef.actorId(),
                actorRef.generation(),
                MESH,
                actorRef.nodeRid()))
            .toCompletableFuture()
            .join();

        long started = System.nanoTime();
        fixture.runtime().handleBoundSessionReplaced(
            actorRef.nodeRid(),
            replacement(actorRef));

        assertTrue(session.replacementEntered.await(5, TimeUnit.SECONDS));
        assertTrue(stream.sessionClosingSendsLatch.await(2, TimeUnit.SECONDS));
        long elapsedMillis = TimeUnit.NANOSECONDS.toMillis(
            System.nanoTime() - started);
        assertEquals(1, session.replacementCallbacks.get());
        stream.enqueue(PEER_A, frame("after-replacement", "{}"));
        Thread.sleep(100);
        assertFalse(session.packetNames.contains("after-replacement"));
        assertTrue(elapsedMillis >= 80, "replacement close must be timer driven");
        assertTrue(elapsedMillis < 2_000, "replacement close exceeded its timer");
    }

    @Test
    void relocationHandlersFenceTheTransportSourceAndAcceptSourceAbort()
        throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("initial", "{}"));
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());
        TestSession session = awaitSession();
        ZLinkActor actor = fixture.actors().getOrCreateLocalActor(
                "relocation-actor", ZLinkActor.class)
            .toCompletableFuture().join().orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context().actors().bind(new ActorRef(
                actorRef.actorId(),
                actorRef.generation(),
                MESH,
                actorRef.nodeRid()))
            .toCompletableFuture().join();
        var codec = new ZLinkServiceM6BWireCodec();
        var relocation = new ZLinkServiceM6BWireCodec.RelocationIdentity(3, 4);
        var coordinator =
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                "actor-owner", 11, actorRef.nodeRid(), 3, "store-v1");
        var owner = new ZLinkServiceM6BWireCodec.SessionOwnerFence(
            RoutingId.from("session-owner-node"),
            3,
            "session-owner",
            5,
            PEER_A,
            1);
        var seal = new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            relocation,
            coordinator,
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actorRef, 3, 7, 11),
            owner);

        assertThrows(CompletionException.class, () ->
            fixture.runtime().handleSessionRelocationSeal(
                    PEER_B,
                    codec.encodeSessionRelocationSeal(seal))
                .toCompletableFuture().join());
        var sealed = codec.decodeSessionRelocationSealed(
            fixture.runtime().handleSessionRelocationSeal(
                    actorRef.nodeRid(),
                    codec.encodeSessionRelocationSeal(seal))
                .toCompletableFuture().join());
        var abort = new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            relocation,
            coordinator,
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorIdentity(
                actorRef.actorId(), actorRef.generation()),
            owner,
            ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
            0,
            7,
            null,
            0);

        assertThrows(CompletionException.class, () ->
            fixture.runtime().handleSessionRelocationRoute(
                    PEER_B,
                    codec.encodeSessionRelocationRoute(abort))
                .toCompletableFuture().join());
        fixture.runtime().handleSessionRelocationRoute(
                actorRef.nodeRid(),
                codec.encodeSessionRelocationRoute(abort))
            .toCompletableFuture().join();
        assertEquals(relocation, sealed.relocation());
    }

    @Test
    void boundSessionReplacementIsIdempotentAndFencedByTheRetiredOwner() throws Exception {
        TestSession.replacementMode = ReplacementMode.SUCCESS;
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());

        TestSession session = awaitSession();
        ZLinkActor actor = fixture.actors().getOrCreateLocalActor(
                "replacement-actor", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context().actors().bind(new ActorRef(
                actorRef.actorId(), actorRef.generation(), MESH, actorRef.nodeRid()))
            .toCompletableFuture()
            .join();

        var command = replacement(actorRef);
        fixture.runtime().handleBoundSessionReplaced(
            actorRef.nodeRid(),
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                command.actorAuthority(),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                    command.retiredSession().sessionOwnerNodeRid(),
                    command.retiredSession().sessionOwnerNodeGeneration(),
                    "stale-owner",
                    command.retiredSession().sessionOwnerLeaseGeneration(),
                    command.retiredSession().sessionRid(),
                    command.retiredSession().retiredBindingGeneration())));
        Thread.sleep(100);
        assertEquals(0, session.replacementCallbacks.get());

        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), command);
        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), command);
        fixture.runtime().handleBoundSessionReplaced(
            actorRef.nodeRid(),
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.BoundSessionReplaced(
                command.actorAuthority(),
                new systems.zlink.framework.runtime.internal.service
                    .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                    command.retiredSession().sessionOwnerNodeRid(),
                    command.retiredSession().sessionOwnerNodeGeneration() + 1,
                    command.retiredSession().sessionOwnerId(),
                    command.retiredSession().sessionOwnerLeaseGeneration(),
                    command.retiredSession().sessionRid(),
                    command.retiredSession().retiredBindingGeneration())));

        assertTrue(session.replacementEntered.await(5, TimeUnit.SECONDS));
        assertTrue(stream.sessionClosingSendsLatch.await(2, TimeUnit.SECONDS));
        assertEquals(1, session.replacementCallbacks.get());
        assertEquals(1, stream.sessionClosingSends.get());
    }

    @Test
    void boundSessionReplacementDeadlineClosesAStalledCallback() throws Exception {
        TestSession.replacementMode = ReplacementMode.PENDING;
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());

        TestSession session = awaitSession();
        ZLinkActor actor = fixture.actors().getOrCreateLocalActor(
                "replacement-actor", ZLinkActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context().actors().bind(new ActorRef(
                actorRef.actorId(), actorRef.generation(), MESH, actorRef.nodeRid()))
            .toCompletableFuture()
            .join();

        long started = System.nanoTime();
        fixture.runtime().handleBoundSessionReplaced(
            actorRef.nodeRid(), replacement(actorRef));

        assertTrue(session.replacementEntered.await(5, TimeUnit.SECONDS));
        assertTrue(stream.sessionClosingSendsLatch.await(8, TimeUnit.SECONDS));
        long elapsedMillis = TimeUnit.NANOSECONDS.toMillis(
            System.nanoTime() - started);
        assertEquals(1, session.replacementCallbacks.get());
        assertTrue(elapsedMillis >= 4_800,
            "a stalled callback must be bounded by the callback deadline");
        assertTrue(elapsedMillis < 8_000,
            "callback deadline did not return to the scheduler promptly");
    }

    private static ReplacementFixture startReplacement(FakeStream stream) {
        stream.enqueue(PEER_A, frame("initial", "{}"));
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("stream")
            .bind("tcp://127.0.0.1:18081")
            .registerSession(TestSession.class);
        ZLinkFrameworkRegistration registration = options.registration();
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ZLinkInternalSpotNode spotNode = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> RoutingId.from("actor-node");
                case "createActor" -> {
                    if (arguments[1] instanceof Message request) {
                        request.close();
                    }
                    yield new ZLinkBackendActorRef(
                        RoutingId.from("actor-node"),
                        (String) arguments[0],
                        7);
                }
                case "localAuthorityLeaseGeneration" -> 11L;
                case "rememberActorAuthority" -> null;
                default -> defaultValue(method.getReturnType());
            });
        ZLinkActorRuntime actors = new ZLinkActorRuntime(
            spotNode,
            Map.of("probe", ProbeFactory.class),
            Duration.ofSeconds(1),
            serializer,
            ZLinkHandlerActivator.reflection());
        actors.setMeshName(MESH);
        ZLinkInternalMeshNode ownerNode = (ZLinkInternalMeshNode)
            Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "routingId" -> RoutingId.from("session-owner-node");
                    case "lifecycleGeneration" -> 3L;
                    case "localAuthorityOwnerId" -> "session-owner";
                    case "localAuthorityLeaseGeneration" -> 5L;
                    default -> defaultValue(method.getReturnType());
                });
        ZLinkStreamRuntime runtime = new ZLinkStreamRuntime(
            new FakeProvider(stream),
            new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
            registration,
            Map.of(),
            Map.of(MESH, ownerNode),
            serializer,
            actors,
            ZLinkHandlerActivator.reflection(),
            ignored -> true,
            null,
            null,
            new FakeContext(),
            false);
        return new ReplacementFixture(runtime, actors);
    }

    private static systems.zlink.framework.runtime.internal.service
        .ZLinkServiceM6BWireCodec.BoundSessionReplaced replacement(
            ZLinkBackendActorRef actorRef) {
        return new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.BoundSessionReplaced(
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.ActorRouteFence(
                actorRef, 3, 7, 11),
            new systems.zlink.framework.runtime.internal.service
                .ZLinkServiceM6BWireCodec.RetiredSessionRouteFence(
                RoutingId.from("session-owner-node"),
                3,
                "session-owner",
                5,
                PEER_A,
                1));
    }

    private record ReplacementFixture(
        ZLinkStreamRuntime runtime,
        ZLinkActorRuntime actors) {
    }

    private enum ReplacementMode {
        NONE,
        SUCCESS,
        FAILURE,
        PENDING
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) {
            return false;
        }
        if (type == char.class) {
            return '\0';
        }
        if (type == byte.class) {
            return (byte) 0;
        }
        if (type == short.class) {
            return (short) 0;
        }
        if (type == int.class) {
            return 0;
        }
        if (type == long.class) {
            return 0L;
        }
        if (type == float.class) {
            return 0F;
        }
        if (type == double.class) {
            return 0D;
        }
        return null;
    }

    private ZLinkStreamRuntime start(FakeStream stream, long hwm) {
        return start(stream, hwm, 64 * 1024);
    }

    private ZLinkStreamRuntime start(FakeStream stream, long hwm, long maxMessageSize) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        var streamNode = options.addStreamNode("stream")
            .bind("tcp://127.0.0.1:18081")
            .registerSession(TestSession.class);
        streamNode.configureSocket().setMaxMessageSize(maxMessageSize);
        ZLinkFrameworkRegistration registration = options.registration();
        lastRegistration = registration;
        FakeProvider provider = new FakeProvider(stream);
        return new ZLinkStreamRuntime(
            provider,
            new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
            registration,
            Map.of(),
            Map.of(),
            new ZLinkJsonMessageSerializer(),
            null,
            ZLinkHandlerActivator.reflection(),
            ignored -> true,
            null,
            null,
            new FakeContext(),
            false,
            (ignoredBackend, ignoredKey) ->
                (ignoredReady, ignoredShutdown) ->
                    CompletableFuture.completedFuture(null));
    }

    private ZLinkStreamRuntime startWithCustomReceiveCodec(FakeStream stream) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("stream")
            .bind("tcp://127.0.0.1:18081")
            .registerSession(TestSession.class);
        ZLinkFrameworkRegistration registration = options.registration();
        ZLinkCodecRegistration codecs = registration.codecs();
        codecs.addSerializer(
            "application/x-wire",
            new WirePayloadSerializer(),
            WirePayload.class::equals);
        codecs.addStreamCodec(
            "application/x-wire", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        ZLinkMessageSerializer serializer = codecs.serializerWithFallback(
            new ZLinkJsonMessageSerializer());
        lastRegistration = registration;
        return new ZLinkStreamRuntime(
            new FakeProvider(stream),
            new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
            registration,
            Map.of(),
            Map.of(),
            serializer,
            null,
            ZLinkHandlerActivator.reflection(),
            ignored -> true,
            null,
            null,
            new FakeContext(),
            false,
            (ignoredBackend, ignoredKey) ->
                (ignoredReady, ignoredShutdown) ->
                    CompletableFuture.completedFuture(null));
    }

    private static TestSession awaitSession() throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            TestSession session = TestSession.lastSession.get();
            if (session != null) {
                return session;
            }
            Thread.sleep(1);
        }
        throw new AssertionError("STREAM session was not created");
    }

    private static void awaitValue(AtomicInteger value, int expected)
        throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (value.get() == expected) {
                return;
            }
            Thread.sleep(1);
        }
        assertEquals(expected, value.get());
    }

    private static byte[] frame(String packetName, String payload) {
        return frame(packetName, ZLinkStreamCodec.JSON, payload);
    }

    private static byte[] frame(
        String packetName,
        ZLinkStreamCodec codec,
        String payload) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            codec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName,
            Map.of());
        return ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header),
            payload.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] controlFrame(String packetName) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.CONTROL,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName,
            Map.of());
        return ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header),
            new byte[0]);
    }

    public static final class TestSession implements ZLinkSession {
        private static final AtomicReference<TestSession> lastSession =
            new AtomicReference<>();
        private static final AtomicInteger createdCount = new AtomicInteger();
        private static volatile boolean holdFirstDispatch;
        private static volatile boolean failNextConstruction;
        private static volatile ReplacementMode replacementMode = ReplacementMode.NONE;
        private static volatile boolean decodeWirePayload;
        private static final AtomicReference<WirePayload> decodedWirePayload =
            new AtomicReference<>();
        private final ZLinkSessionContext context;
        private final CountDownLatch dispatchLatch = new CountDownLatch(1);
        private final CountDownLatch secondDispatchLatch = new CountDownLatch(1);
        private final CompletableFuture<Void> firstDispatch = new CompletableFuture<>();
        private final AtomicInteger dispatchCount = new AtomicInteger();
        private final AtomicInteger replacementCallbacks = new AtomicInteger();
        private final CountDownLatch replacementEntered = new CountDownLatch(1);
        private final CompletableFuture<Void> replacementCompletion =
            new CompletableFuture<>();
        private final List<String> packetNames =
            Collections.synchronizedList(new ArrayList<>());

        public TestSession(ZLinkSessionContext context) {
            if (failNextConstruction) {
                failNextConstruction = false;
                throw new IllegalStateException("test session construction failure");
            }
            this.context = context;
            createdCount.incrementAndGet();
            lastSession.set(this);
        }

        @Override public ZLinkSessionContext context() { return context; }
        @Override public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onActorBindingReplaced(String actorId) {
            replacementCallbacks.incrementAndGet();
            replacementEntered.countDown();
            return switch (replacementMode) {
                case SUCCESS, NONE -> CompletableFuture.completedFuture(null);
                case FAILURE -> CompletableFuture.failedFuture(
                    new IllegalStateException("replacement callback failure"));
                case PENDING -> replacementCompletion;
            };
        }
        @Override public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
            if (decodeWirePayload) {
                decodedWirePayload.set(payload.decode(WirePayload.class));
            }
            int count = dispatchCount.incrementAndGet();
            packetNames.add(dispatch.packetName());
            dispatchLatch.countDown();
            if (count == 1 && holdFirstDispatch) {
                return firstDispatch;
            }
            secondDispatchLatch.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    private record WirePayload(String marker) {
    }

    private static final class WirePayloadSerializer
        implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from(
                "CUSTOM".getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            return type.cast(new WirePayload("CUSTOM"));
        }
    }

    public static final class ProbeFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ProbeActor(context));
        }
    }

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {
    }

    private static final class FakeProvider implements ZLinkBackendAdapterProvider {
        private final FakeStream stream;

        private FakeProvider(FakeStream stream) {
            this.stream = stream;
        }

        @Override public ZLinkChannelBackendAdapter createChannelAdapter(
            ZLinkBackendAdapterOptions options) { throw new UnsupportedOperationException(); }
        @Override public ZLinkSpotBackendAdapter createSpotAdapter(
            ZLinkBackendAdapterOptions options) { throw new UnsupportedOperationException(); }
        @Override public ZLinkStreamBackendAdapter createStreamAdapter(
            ZLinkBackendAdapterOptions options) {
            return (context, meshNode) -> stream;
        }
        @Override public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
            ZLinkBackendAdapterOptions options) { throw new UnsupportedOperationException(); }
    }

    private static final class FakeContext implements ZLinkBackendContext {
        @Override public String name() { return "fake-context"; }
        @Override public void close() { }
        @Override public void shutdown() { }
    }

    private static final class FakeStream implements ZLinkBackendStreamSocket {
        private final Queue<ZLinkBackendStreamReceived> received =
            new ConcurrentLinkedQueue<>();
        private final AtomicInteger successfulReceives = new AtomicInteger();
        private final AtomicInteger readinessWaits = new AtomicInteger();
        private final AtomicBoolean receivePermit = new AtomicBoolean();
        private final AtomicInteger sessionClosingSends = new AtomicInteger();
        private final CountDownLatch sessionClosingSendsLatch = new CountDownLatch(1);
        private final AtomicReference<RoutingId> disconnectedPeer =
            new AtomicReference<>();
        private final CountDownLatch firstReceiveEntered = new CountDownLatch(1);
        private final CountDownLatch firstReceiveRelease = new CountDownLatch(1);
        private volatile boolean blockFirstReceive;
        private volatile boolean ignoreFirstReceiveInterrupt;
        private volatile boolean failHeartbeatPongSend;
        private final AtomicInteger closeCalls = new AtomicInteger();
        private boolean notificationsEnabled;
        private boolean boundAfterNotifications;
        private ZLinkBackendStreamErrorHandler errorHandler;

        private void enqueue(RoutingId routingId, byte[] bytes) {
            Message part = Message.from(bytes);
            received.add(new ZLinkBackendStreamReceived(
                Optional.of(routingId),
                List.of(part),
                part::close));
        }

        private void enqueueTracked(
            RoutingId routingId,
            byte[] bytes,
            AtomicInteger ownerCloses) {
            Message part = Message.from(bytes);
            received.add(new ZLinkBackendStreamReceived(
                Optional.of(routingId),
                List.of(part),
                () -> {
                    part.close();
                    ownerCloses.incrementAndGet();
                }));
        }

        private void enqueueEmptyParts(RoutingId routingId) {
            received.add(new ZLinkBackendStreamReceived(
                Optional.of(routingId),
                List.of(),
                () -> { }));
        }

        private void blockFirstReceive() {
            blockFirstReceive = true;
        }

        private void blockFirstReceiveIgnoringInterrupt() {
            blockFirstReceive = true;
            ignoreFirstReceiveInterrupt = true;
        }

        @Override public String name() { return "fake-stream"; }
        @Override public void close() { closeCalls.incrementAndGet(); }
        @Override public void bind(String endpoint) {
            boundAfterNotifications = notificationsEnabled;
        }
        @Override public void setTlsServer(
            String certificatePath, String keyPath,
            boolean requireClientCertificate) { }
        @Override public void setMaxMessageSize(long value) { }
        @Override public void enableNotifications() { notificationsEnabled = true; }
        @Override public boolean waitForReadable(Duration timeout) {
            readinessWaits.incrementAndGet();
            boolean readable = !received.isEmpty();
            if (readable) {
                receivePermit.set(true);
            }
            return readable;
        }
        @Override public ZLinkBackendStreamReceived recv() {
            if (!receivePermit.compareAndSet(true, false)) {
                throw new AssertionError("STREAM recv was called without readiness");
            }
            if (blockFirstReceive) {
                blockFirstReceive = false;
                firstReceiveEntered.countDown();
                try {
                    if (ignoreFirstReceiveInterrupt) {
                        while (true) {
                            try {
                                firstReceiveRelease.await();
                                break;
                            } catch (InterruptedException ignored) {
                                // The fake backend models a native receive that
                                // cannot be cancelled by an executor interrupt.
                            }
                        }
                    } else if (!firstReceiveRelease.await(5, TimeUnit.SECONDS)) {
                        throw new AssertionError("first STREAM receive was not released");
                    }
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    throw new AssertionError("first STREAM receive was interrupted", interrupted);
                }
            }
            ZLinkBackendStreamReceived next = received.poll();
            if (next != null) {
                successfulReceives.incrementAndGet();
            }
            return next;
        }
        @Override public void disconnectPeer(RoutingId routingId) {
            disconnectedPeer.set(routingId);
        }
        @Override public void onTransportError(ZLinkBackendStreamErrorHandler handler) {
            errorHandler = handler;
        }
        @Override public void startSessionService() { }
        @Override public boolean send(
            RoutingId routingId, List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean send(
            RoutingId routingId, String packetName,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean send(
            RoutingId routingId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) {
            if (failHeartbeatPongSend
                && "$zlink.heartbeat.pong".equals(header.packetName())) {
                return false;
            }
            if ("session-closing".equals(header.packetName())) {
                sessionClosingSends.incrementAndGet();
                sessionClosingSendsLatch.countDown();
            }
            return true;
        }
        @Override public boolean reply(
            RoutingId routingId, long requestSeq, String packetName,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean reply(
            RoutingId routingId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public ZLinkBackendActorBindOperation
            bindActor(RoutingId sessionRid,
                ZLinkBackendActorRef actor) {
            return timeout -> CompletableFuture.completedFuture(null);
        }
        @Override public ZLinkBackendActorUnbindOperation
            unbindActor(RoutingId sessionRid, String actorId) {
            return timeout -> CompletableFuture.completedFuture(null);
        }
        @Override public boolean sendBoundActor(
            RoutingId sessionRid, String actorId,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean relayBoundActor(
            RoutingId sessionRid, String actorId,
            ZLinkStreamHeader header, List<Message> parts,
            SendFlags flags) { return true; }
    }
}
