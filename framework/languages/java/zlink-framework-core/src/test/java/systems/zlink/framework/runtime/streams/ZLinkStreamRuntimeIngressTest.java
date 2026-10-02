package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.lang.reflect.Proxy;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.Level;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkStreamRuntimeIngressTest {
    private static final RoutingId PEER_A = RoutingId.from("peer-a");
    private static final RoutingId PEER_B = RoutingId.from("peer-b");
    private static final String MESH = "replacement-mesh";
    private static final String REPLACEMENT_ACTOR = "replacement-actor";
    private static final String HEARTBEAT_PING = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG = "$zlink.heartbeat.pong";
    private final List<ZLinkStreamRuntime> runtimes = new ArrayList<>();
    private ZLinkFrameworkRegistration lastRegistration;

    @Test
    void heartbeatContractConnectionWithoutInboundClosesAtTimeout() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        stream.errorHandler.handle(PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready");
        clock.set(Duration.ofSeconds(5).toNanos() - 1);
        runtime.checkSessionLiveness();
        assertEquals(0, stream.sessionClosingSends.get());
        clock.incrementAndGet();
        List<String> reasons = new ArrayList<>();
        try (AutoCloseable ignored = installClosedMetricSink(reasons)) {
            runtime.checkSessionLiveness();
            assertEquals(1, stream.sessionClosingSends.get());
            assertEquals(List.of("heartbeat_timeout"), reasons);
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {"application", "pong"})
    void heartbeatContractAnyInboundExtendsTimeout(String inbound) throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        dispatchDeterministic(runtime, "initial", false);
        for (int index = 1; index <= 3; index++) {
            clock.addAndGet(Duration.ofSeconds(5).toNanos() - 1);
            dispatchDeterministic(
                    runtime,
                    inbound.equals("pong") ? HEARTBEAT_PONG : "activity",
                    inbound.equals("pong"));
            clock.incrementAndGet();
            runtime.checkSessionLiveness();
            assertEquals(0, stream.sessionClosingSends.get());
        }
        clock.addAndGet(Duration.ofSeconds(5).toNanos());
        List<String> reasons = new ArrayList<>();
        try (AutoCloseable ignored = installClosedMetricSink(reasons)) {
            runtime.checkSessionLiveness();
            assertEquals(1, stream.sessionClosingSends.get());
            assertEquals(List.of("heartbeat_timeout"), reasons);
        }
    }

    @Test
    void heartbeatContractDisabledActiveTimerStillRepliesToPing() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        stream.deferHeartbeatPongSend = true;
        dispatchDeterministic(runtime, HEARTBEAT_PING, true);
        assertEquals(0, stream.heartbeatPongAsyncAttempted.getCount());
        stream.deferredHeartbeatPong.complete(null);
    }

    @Test
    void heartbeatContractPingsRepeatWithoutPong() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        stream.errorHandler.handle(PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready");
        for (int second = 1; second <= 2; second++) {
            clock.set(Duration.ofSeconds(second).toNanos());
            runtime.checkSessionLiveness();
        }
        assertEquals(0, stream.heartbeatPingAttempted.getCount());
        assertEquals(0, stream.sessionClosingSends.get());
    }

    @Test
    void heartbeatContractConstructionRunsOutsideStateLane() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        TestSession.constructionHook =
                () ->
                        assertNull(
                                systems.zlink.framework.runtime.internal.execution.ZLinkStateLane
                                        .current());
        stream.errorHandler.handle(PEER_B, MonitorEventType.CONNECTION_READY, 0, "ready");
        dispatchDeterministic(runtime, "initial", false);
    }

    @Test
    void heartbeatContractReadyAndDataPublishOneSession() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        var ready =
                CompletableFuture.runAsync(
                        () ->
                                stream.errorHandler.handle(
                                        PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready"));
        dispatchDeterministic(runtime, "initial", false);
        ready.get(5, TimeUnit.SECONDS);
        TestSession original = TestSession.lastSession.get();
        clock.set(Duration.ofSeconds(4).toNanos());
        dispatchDeterministic(runtime, "activity", false);
        clock.set(Duration.ofSeconds(8).toNanos());
        stream.errorHandler.handle(PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready");
        runtime.checkSessionLiveness();
        assertSame(original, TestSession.lastSession.get());
        assertEquals(0, stream.sessionClosingSends.get());
        clock.set(Duration.ofSeconds(9).toNanos());
        runtime.checkSessionLiveness();
        assertEquals(1, stream.sessionClosingSends.get());
    }

    @Test
    void heartbeatContractSlowConstructionPreservesApplicationIdleStart() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        TestSession.constructionHook = () -> clock.set(Duration.ofSeconds(4).toNanos());
        stream.errorHandler.handle(PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready");
        for (int second = 4; second <= 28; second += 4) {
            clock.set(Duration.ofSeconds(second).toNanos());
            dispatchDeterministic(runtime, HEARTBEAT_PONG, true);
            runtime.checkSessionLiveness();
        }
        clock.set(Duration.ofSeconds(30).toNanos());
        dispatchDeterministic(runtime, HEARTBEAT_PONG, true);
        runtime.checkSessionLiveness();
        assertEquals(0, stream.sessionClosingSends.get());
        clock.set(Duration.ofSeconds(34).toNanos());
        dispatchDeterministic(runtime, HEARTBEAT_PONG, true);
        List<String> reasons = new ArrayList<>();
        try (AutoCloseable ignored = installClosedMetricSink(reasons)) {
            runtime.checkSessionLiveness();
            assertEquals(List.of("idle_timeout"), reasons);
        }
    }

    @Test
    void heartbeatContractFailedReadyConstructionReleasesContext() throws Exception {
        FakeStream stream = new FakeStream();
        var clock = new java.util.concurrent.atomic.AtomicLong();
        ZLinkStreamRuntime runtime = startDeterministic(stream, clock);
        var contextsField = ZLinkStreamRuntime.class.getDeclaredField("sessionContexts");
        contextsField.setAccessible(true);
        var contexts = (java.util.Set<?>) contextsField.get(runtime);
        TestSession.failNextConstruction = true;
        assertThrows(
                RuntimeException.class,
                () ->
                        stream.errorHandler.handle(
                                PEER_A, MonitorEventType.CONNECTION_READY, 0, "ready"));
        assertEquals(0, contexts.size());
        stream.errorHandler.handle(PEER_B, MonitorEventType.CONNECTION_READY, 0, "ready");
        assertEquals(1, contexts.size());
    }

    private ZLinkStreamRuntime startDeterministic(
            FakeStream stream, java.util.concurrent.atomic.AtomicLong clock) {
        var registration = streamRegistration(new DefaultZLinkFrameworkOptions(), 64 * 1024);
        lastRegistration = registration;
        var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        var runtime =
                new ZLinkStreamRuntime(
                                new FakeProvider(stream),
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
                                                CompletableFuture.completedFuture(null),
                                clock::get,
                                () -> scheduler)
                        .start();
        scheduler.shutdownNow();
        runtimes.add(runtime);
        return runtime;
    }

    private void dispatchDeterministic(ZLinkStreamRuntime runtime, String packet, boolean control)
            throws Exception {
        var method =
                ZLinkStreamRuntime.class.getDeclaredMethod(
                        "dispatchToSession",
                        StreamNodeRegistration.class,
                        RoutingId.class,
                        ZLinkStreamHeader.class,
                        Message.class);
        method.setAccessible(true);
        var header =
                new ZLinkStreamHeader(
                        control ? ZLinkStreamMessageKind.CONTROL : ZLinkStreamMessageKind.SEND,
                        control ? ZLinkStreamCodec.RAW : ZLinkStreamCodec.JSON,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.empty(),
                        packet,
                        Map.of());
        try (Message payload =
                Message.from(control ? new byte[0] : "{}".getBytes(StandardCharsets.UTF_8))) {
            ((CompletionStage<?>)
                            method.invoke(
                                    runtime,
                                    lastRegistration.streamNodes().getFirst(),
                                    PEER_A,
                                    header,
                                    payload))
                    .toCompletableFuture()
                    .get(5, TimeUnit.SECONDS);
        }
    }

    @Test
    void receiveThreadRejectsPublicBlockingSubmitAndLaterCompletionStillRuns() throws Exception {
        FakeStream stream = new FakeStream();
        CompletableFuture<Void> admission = new CompletableFuture<>();
        CompletableFuture<Void> checked = new CompletableFuture<>();
        AtomicInteger submissions = new AtomicInteger();
        ZLinkSendCall call =
                () -> {
                    submissions.incrementAndGet();
                    return admission;
                };
        stream.readabilityObserver =
                () -> {
                    try {
                        assertEquals(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                assertThrows(ZLinkFrameworkException.class, call::submit_sync)
                                        .kind());
                        assertEquals(0, submissions.get());
                        checked.complete(null);
                    } catch (Throwable failure) {
                        checked.completeExceptionally(failure);
                    }
                };
        runtimes.add(start(stream, 0));
        try (var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
                var completions = new ZLinkServiceOperationRegistry(scheduler)) {
            try {
                checked.get(3, TimeUnit.SECONDS);
                CompletableFuture<Void> terminal = new CompletableFuture<>();
                ZLinkSendCall following =
                        () ->
                                completions.submit(
                                        UUID.randomUUID(),
                                        Duration.ofSeconds(3),
                                        () -> terminal,
                                        ignored -> {});
                var stage = following.submit().toCompletableFuture();
                terminal.complete(null);
                stage.get(3, TimeUnit.SECONDS);
            } finally {
                admission.complete(null);
            }
        }
    }

    @AfterEach
    void resetSessionProbe() {
        TestSession.holdFirstDispatch = false;
        TestSession.failNextConstruction = false;
        TestSession.constructionHook = null;
        TestSession.replacementMode = ReplacementMode.NONE;
        TestSession.decodeWirePayload = false;
        TestSession.replyOnDispatch = false;
        TestSession.decodedWirePayload.set(null);
        TestSession.created.clear();
        TestSession.createdCount.set(0);
        TestSession.lastSession.set(null);
        TestSession.connected = new CompletableFuture<>();
        runtimes.forEach(runtime -> runtime.closeAsync().toCompletableFuture().join());
        runtimes.clear();
        lastRegistration = null;
    }

    @ParameterizedTest
    @ValueSource(strings = {"DISCONNECTED", "transport failure"})
    void reportsTransportCallbacksThroughPublicSessionMethods(String message) throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("initial", "{}"));
        runtimes.add(start(stream, 0));
        TestSession session = TestSession.connected.get(5, TimeUnit.SECONDS);
        stream.errorHandler.handle(
                PEER_A,
                MonitorEventType.DISCONNECTED.name().equals(message)
                        ? MonitorEventType.DISCONNECTED
                        : null,
                0,
                message);
        List<String> expected =
                Map.of(
                                "DISCONNECTED", List.of("disconnected"),
                                "transport failure",
                                        List.of("error:transport failure", "disconnected"))
                        .get(message);
        assertEquals(expected, session.transportClosed.get(5, TimeUnit.SECONDS));
    }

    @Test
    void transportErrorMessageDoesNotOverrideTypedMonitorEvent() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("initial", "{}"));
        runtimes.add(start(stream, 0));
        TestSession session = TestSession.connected.get(5, TimeUnit.SECONDS);
        stream.errorHandler.handle(PEER_A, null, 0, "DISCONNECTED");
        assertEquals(
                List.of("error:DISCONNECTED", "disconnected"),
                session.transportClosed.get(5, TimeUnit.SECONDS));
    }

    @Test
    void pullsOneCompletePacketAndPreservesTheSourceRoutingId() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("packet", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(PEER_A, session.context.routingId().orElseThrow());
        assertTrue(stream.successfulReceives.get() >= 1);
        assertTrue(stream.readinessWaits.get() >= 1);
        assertEquals(List.of("packet"), session.packetNames);
    }

    @Test
    void streamRequestFlowRecordsCarrySessionIdentity() throws Exception {
        TestSession.replyOnDispatch = true;
        FakeStream stream = new FakeStream();
        ZLinkStreamHeader request =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.REQUEST,
                        ZLinkStreamCodec.JSON,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.of(17L),
                        "packet",
                        Map.of());
        stream.enqueue(
                PEER_A,
                ZLinkStreamFrameCodec.encode(
                        ZLinkStreamHeaderCodec.encode(request),
                        "{}".getBytes(StandardCharsets.UTF_8)));
        Logger logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        List<String> lines = new CopyOnWriteArrayList<>();
        CountDownLatch replied = new CountDownLatch(1);
        Handler handler =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        lines.add(record.getMessage());
                        if (record.getMessage().contains(" phase=replied")) {
                            replied.countDown();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        boolean parentHandlers = logger.getUseParentHandlers();
        Level level = logger.getLevel();
        logger.addHandler(handler);
        logger.setUseParentHandlers(false);
        logger.setLevel(Level.ALL);
        try {
            ZLinkStreamRuntime runtime =
                    start(
                            stream,
                            0,
                            64 * 1024,
                            registration ->
                                    registration
                                            .dispatchOptions()
                                            .messageFlow(ZLinkMessageFlowLogMode.NORMAL));
            runtimes.add(runtime);
            TestSession session = awaitSession();
            assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
            assertTrue(replied.await(5, TimeUnit.SECONDS));
            String identity = " session=" + session.context.routingId().orElseThrow().toHex();
            for (String phase : List.of("received", "admitted", "dispatched")) {
                assertTrue(
                        lines.stream()
                                .anyMatch(
                                        line ->
                                                line.contains(" phase=" + phase)
                                                        && line.contains(" surface=stream")
                                                        && line.contains(identity)),
                        () -> "missing session on " + phase + ": " + lines);
            }
            assertTrue(
                    lines.stream()
                            .anyMatch(
                                    line ->
                                            line.contains(" phase=replied")
                                                    && line.contains(" surface=stream")
                                                    && line.contains(identity)),
                    () -> "missing session on reply: " + lines);
        } finally {
            logger.removeHandler(handler);
            logger.setUseParentHandlers(parentHandlers);
            logger.setLevel(level);
        }
    }

    @Test
    void sessionPayloadDecodeUsesTheSerializerMappedByTheWireCodec() throws Exception {
        TestSession.decodeWirePayload = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("custom", ZLinkStreamCodec.PROTOBUF, "wire"));

        ZLinkStreamRuntime runtime = startWithCustomReceiveCodec(stream);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(new WirePayload("CUSTOM"), TestSession.decodedWirePayload.get());
    }

    @Test
    void continuesReceivingAcrossSerializedSessionDispatch() throws Exception {
        TestSession.holdFirstDispatch = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("first", "a"));
        stream.enqueue(PEER_A, frame("second", "b"));

        ZLinkStreamRuntime runtime = start(stream, 1);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        try {
            assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
            assertEquals(1, session.dispatchCount.get());
            assertFalse(session.firstDispatch.isDone());
            awaitValue(stream.successfulReceives, 2);
            assertEquals(0, stream.zeroReadinessWaits.get());
            assertFalse(session.secondDispatchLatch.await(100, TimeUnit.MILLISECONDS));
            assertEquals(1, session.dispatchCount.get());
        } finally {
            session.firstDispatch.complete(null);
        }
        assertTrue(session.secondDispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(2, session.dispatchCount.get());
        assertEquals(List.of("first", "second"), session.packetNames);
    }

    @Test
    void retainsEachPacketOwnerThroughItsHandlerTerminalAndThenProgresses() throws Exception {
        TestSession.holdFirstDispatch = true;
        AtomicInteger ownerCloses = new AtomicInteger();
        FakeStream stream = new FakeStream();
        stream.enqueueTracked(PEER_A, frame("first-retained", "a"), ownerCloses);
        stream.enqueueTracked(PEER_A, frame("second-retained", "b"), ownerCloses);

        ZLinkStreamRuntime runtime = start(stream, 1);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertFalse(session.firstDispatch.isDone());
        assertEquals(0, ownerCloses.get());

        session.firstDispatch.complete(null);
        assertTrue(session.secondDispatchLatch.await(5, TimeUnit.SECONDS));
        awaitValue(ownerCloses, 2);
        assertEquals(List.of("first-retained", "second-retained"), session.packetNames);

        stream.enqueue(PEER_A, frame("after-release", "c"));
        awaitValue(session.dispatchCount, 3);
        assertEquals(
                List.of("first-retained", "second-retained", "after-release"), session.packetNames);
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
    void isolatesPeerWithNoMessagePartsAndContinuesReceivingAnotherPeer() throws Exception {
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
    void oversizePacketRecordsEmsgsizeAndDisconnectsThePeer() throws Exception {
        FakeStream stream = new FakeStream();
        byte[] oversize = frame("oversize", "x".repeat(512));
        stream.enqueue(PEER_A, oversize);
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0, 256);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(List.of("good"), session.packetNames);
        assertEquals(PEER_A, stream.disconnectedPeer.getNow(null));
    }

    @Test
    void heartbeatBackpressureDoesNotIsolateThePeer() throws Exception {
        FakeStream stream = new FakeStream();
        stream.failHeartbeatPongSend = true;
        stream.enqueue(PEER_A, controlFrame(HEARTBEAT_PING));
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
    void pendingHeartbeatPongAdmissionDoesNotBlockTheReceiveOwner() throws Exception {
        FakeStream stream = new FakeStream();
        stream.deferHeartbeatPongSend = true;
        stream.enqueue(PEER_A, controlFrame(HEARTBEAT_PING));
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertTrue(stream.heartbeatPongAsyncAttempted.await(5, TimeUnit.SECONDS));
        assertEquals(List.of("good"), session.packetNames);
        assertEquals(0, stream.synchronousHeartbeatPongSends.get());
        assertFalse(stream.deferredHeartbeatPong.isDone());
        stream.deferredHeartbeatPong.complete(null);
    }

    @Test
    void heartbeatTransportFailureDoesNotStopLivenessChecks() throws Exception {
        FakeStream stream = new FakeStream();
        stream.failHeartbeatPingSend = true;
        stream.enqueue(PEER_A, frame("initial", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        awaitSession();
        assertTrue(stream.heartbeatPingAttempted.await(5, TimeUnit.SECONDS));
    }

    @Test
    void livenessTimeoutsAreIncludedInClosedConnectionMetrics() throws Exception {
        assertLivenessCloseReason(new FakeStream(), "heartbeat_timeout");
        FakeStream idleStream =
                new FakeStream() {
                    @Override
                    public CompletionStage<Void> sendAsync(
                            RoutingId routingId, ZLinkStreamHeader header, List<Message> parts) {
                        if (HEARTBEAT_PING.equals(header.packetName())) {
                            ((FakeStream) this).enqueue(routingId, controlFrame(HEARTBEAT_PONG));
                        }
                        return super.sendAsync(routingId, header, parts);
                    }
                };
        assertLivenessCloseReason(idleStream, "idle_timeout");
    }

    @Test
    void drainedCloseUsesServerDrainMetricReason() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("initial", "{}"));
        List<String> closeReasons = Collections.synchronizedList(new ArrayList<>());
        try (AutoCloseable ignored = installClosedMetricSink(closeReasons)) {
            ZLinkStreamRuntime runtime = start(stream, 0);
            runtimes.add(runtime);

            TestSession session = awaitSession();
            assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
            runtime.beginDrain();
            runtime.closeAsync().toCompletableFuture().join();

            assertEquals(List.of("server_drain"), closeReasons);
        }
    }

    @Test
    void sessionConstructionFailureDoesNotStopAnotherPeer() throws Exception {
        TestSession.failNextConstruction = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("first-construction", "{}"));
        stream.enqueue(PEER_B, frame("good", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        assertEquals(1, TestSession.createdCount.get());
        assertEquals(PEER_B, session.context.routingId().orElseThrow());
        assertEquals(List.of("good"), session.packetNames);
        var contexts = ZLinkStreamRuntime.class.getDeclaredField("sessionContexts");
        contexts.setAccessible(true);
        assertEquals(1, ((java.util.Set<?>) contexts.get(runtime)).size());
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

        assertTrue(stream.sessionClosingSendsLatch.await(5, TimeUnit.SECONDS));
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
        CompletableFuture<Void> close =
                CompletableFuture.runAsync(() -> runtime.closeAsync().toCompletableFuture().join());
        assertThrows(
                java.util.concurrent.TimeoutException.class,
                () -> close.get(2_200, TimeUnit.MILLISECONDS));
        assertEquals(0, stream.closeCalls.get());

        stream.firstReceiveRelease.countDown();
        close.get(5, TimeUnit.SECONDS);
        assertEquals(1, stream.closeCalls.get());
    }

    @Test
    void permitWaitReleasesTheReceiveThreadAndTheGrantResumesReceive() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("after-grant", "{}"));
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        ZLinkApplicationJobQueue queue = null;
        ZLinkApplicationJobQueue.Permit held = null;
        try {
            ZLinkFrameworkRegistration registration = streamRegistration(options, 64 * 1024);
            queue = registration.applicationJobQueue();
            held = queue.acquire().toCompletableFuture().join();
            ZLinkStreamRuntime runtime = start(stream, registration);
            runtimes.add(runtime);
            ZLinkApplicationJobQueue waiting = queue;
            awaitCondition(() -> waiting.snapshot().capacityWaiters() == 1);

            awaitCondition(
                    () -> !anyThreadRuns(ZLinkStreamRuntime.class.getName() + "$StreamReceiveLoop"),
                    "a thread still runs the STREAM receive loop while its permit is pending");
            assertEquals(0, stream.successfulReceives.get());

            held.close();
            held = null;
            TestSession session = awaitSession();
            assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
            assertEquals(List.of("after-grant"), session.packetNames);
        } finally {
            if (held != null) {
                held.close();
            }
        }
    }

    @Test
    void closingStreamReceiveOwnerCancelsItsFifoWaitWithoutResuming() throws Exception {
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("after-close", "{}"));
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.configureInboundDispatch().setMaxQueuedApplicationJobs(1);
        ZLinkFrameworkRegistration registration = streamRegistration(options, 64 * 1024);
        ZLinkApplicationJobQueue queue = registration.applicationJobQueue();
        ZLinkApplicationJobQueue.Permit held = queue.acquire().toCompletableFuture().join();
        try {
            ZLinkStreamRuntime runtime = start(stream, registration);
            runtimes.add(runtime);
            awaitCondition(() -> queue.snapshot().capacityWaiters() == 1);
            runtime.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
            awaitCondition(() -> queue.snapshot().capacityWaiters() == 0);
            held.close();
            try (var next = queue.acquire().toCompletableFuture().get(1, TimeUnit.SECONDS)) {
                assertEquals(0, stream.successfulReceives.get());
                assertEquals(1, queue.snapshot().permitsInUse());
            }
            assertEquals(0, queue.snapshot().permitsInUse());
            assertEquals(0, TestSession.createdCount.get());
        } finally {
            held.close();
        }
    }

    @Test
    void samePeerPacketsKeepOrderWhileAnotherPeerProgresses() throws Exception {
        TestSession.holdFirstDispatch = true;
        FakeStream stream = new FakeStream();
        stream.enqueue(PEER_A, frame("a-first", "{}"));
        stream.enqueue(PEER_A, frame("a-second", "{}"));
        stream.enqueue(PEER_B, frame("b-first", "{}"));

        ZLinkStreamRuntime runtime = start(stream, 0);
        runtimes.add(runtime);

        try {
            awaitCondition(() -> TestSession.created.size() == 2);
            TestSession first = TestSession.created.get(0);
            TestSession second = TestSession.created.get(1);
            assertEquals(PEER_A, first.context.routingId().orElseThrow());
            assertEquals(PEER_B, second.context.routingId().orElseThrow());
            assertTrue(second.dispatchLatch.await(5, TimeUnit.SECONDS));
            assertEquals(List.of("b-first"), second.packetNames);
            assertEquals(List.of("a-first"), first.packetNames);
            assertEquals(2, TestSession.createdCount.get(), "one Session per peer");

            first.firstDispatch.complete(null);
            assertTrue(first.secondDispatchLatch.await(5, TimeUnit.SECONDS));
            assertEquals(List.of("a-first", "a-second"), first.packetNames);
        } finally {
            // Held handlers would keep their Session queues from draining at close.
            TestSession.created.forEach(session -> session.firstDispatch.complete(null));
        }
    }

    @Test
    void boundSessionReplacementRunsCallbackBeforeADeferredClose() throws Exception {
        TestSession.replacementMode = ReplacementMode.FAILURE;
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        ZLinkActor actor =
                fixture.actors()
                        .getOrCreateLocalActor(REPLACEMENT_ACTOR, ZLinkActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context()
                .actors()
                .bind(
                        new ActorRef(
                                actorRef.actorId(),
                                actorRef.generation(),
                                MESH,
                                actorRef.nodeRid()))
                .toCompletableFuture()
                .join();

        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), replacement(actorRef));

        assertTrue(session.replacementEntered.await(5, TimeUnit.SECONDS));
        assertEquals(PEER_A, stream.disconnectedPeer.get(5, TimeUnit.SECONDS));
        assertEquals(1, session.replacementCallbacks.get());
        assertEquals(1, stream.disconnectCalls.get());
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void boundSessionReplacementDisconnectsAfterCallbackTerminal(boolean completeClosingControlSend)
            throws Exception {
        TestSession.replacementMode = ReplacementMode.SUCCESS;
        FakeStream stream = new FakeStream();
        stream.completeClosingControlSend = completeClosingControlSend;
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());
        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        ZLinkActor actor =
                fixture.actors()
                        .getOrCreateLocalActor(REPLACEMENT_ACTOR, ZLinkActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context()
                .actors()
                .bind(
                        new ActorRef(
                                actorRef.actorId(),
                                actorRef.generation(),
                                MESH,
                                actorRef.nodeRid()))
                .toCompletableFuture()
                .join();

        CompletableFuture<Void> ingressBeforeTerminal =
                session.replacementCompletion.thenRun(
                        () -> {
                            assertEquals(0, stream.disconnectCalls.get());
                            stream.enqueue(PEER_A, frame("during-replacement", "{}"));
                        });
        assertEquals(0, stream.disconnectCalls.get());
        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), replacement(actorRef));
        try {
            long terminalAt = session.replacementCompletion.get(5, TimeUnit.SECONDS);
            ingressBeforeTerminal.join();
            assertEquals(PEER_A, stream.disconnectedPeer.get(5, TimeUnit.SECONDS));
            long closeDelay = stream.disconnectStartedAt - terminalAt;
            assertTrue(
                    closeDelay >= TimeUnit.MILLISECONDS.toNanos(100),
                    "callback terminal must precede transport close by 100 ms");
            assertEquals(1, session.replacementCallbacks.get());
            assertEquals(2, stream.successfulReceives.get());
            assertEquals(2, stream.sessionClosingSends.get());
            assertEquals(List.of("initial"), session.packetNames);
            assertEquals(1, stream.disconnectCalls.get());
        } finally {
            stream.closingControlCompletion.complete(null);
        }
    }

    @Test
    void relocationHandlersFenceTheTransportSourceAndAcceptSourceAbort() throws Exception {
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());
        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        ZLinkActor actor =
                fixture.actors()
                        .getOrCreateLocalActor("relocation-actor", ZLinkActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context()
                .actors()
                .bind(
                        new ActorRef(
                                actorRef.actorId(),
                                actorRef.generation(),
                                MESH,
                                actorRef.nodeRid()))
                .toCompletableFuture()
                .join();
        var codec = new ZLinkServiceM6BWireCodec();
        var relocation = new ZLinkServiceM6BWireCodec.RelocationIdentity(3, 4);
        var coordinator =
                new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                        "actor-owner", 11, actorRef.nodeRid(), 3, "store-v1");
        var owner =
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        RoutingId.from("session-owner-node"), 3, "session-owner", 5, PEER_A, 1);
        var seal =
                new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
                        relocation,
                        coordinator,
                        ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(actorRef, 3, 7, 11),
                        owner);

        assertThrows(
                CompletionException.class,
                () ->
                        fixture.runtime()
                                .handleSessionRelocationSeal(
                                        PEER_B, codec.encodeSessionRelocationSeal(seal))
                                .toCompletableFuture()
                                .join());
        var sealed =
                codec.decodeSessionRelocationSealed(
                        fixture.runtime()
                                .handleSessionRelocationSeal(
                                        actorRef.nodeRid(), codec.encodeSessionRelocationSeal(seal))
                                .toCompletableFuture()
                                .join());
        var abort =
                new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
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
                        0,
                        0);

        assertThrows(
                CompletionException.class,
                () ->
                        fixture.runtime()
                                .handleSessionRelocationRoute(
                                        PEER_B, codec.encodeSessionRelocationRoute(abort))
                                .toCompletableFuture()
                                .join());
        fixture.runtime()
                .handleSessionRelocationRoute(
                        actorRef.nodeRid(), codec.encodeSessionRelocationRoute(abort))
                .toCompletableFuture()
                .join();
        assertEquals(relocation, sealed.relocation());
    }

    @Test
    void boundSessionReplacementIsIdempotentAndFencedByTheRetiredOwner() throws Exception {
        TestSession.replacementMode = ReplacementMode.SUCCESS;
        FakeStream stream = new FakeStream();
        ReplacementFixture fixture = startReplacement(stream);
        runtimes.add(fixture.runtime());

        TestSession session = awaitSession();
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        ZLinkActor actor =
                fixture.actors()
                        .getOrCreateLocalActor(REPLACEMENT_ACTOR, ZLinkActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context()
                .actors()
                .bind(
                        new ActorRef(
                                actorRef.actorId(),
                                actorRef.generation(),
                                MESH,
                                actorRef.nodeRid()))
                .toCompletableFuture()
                .join();

        var command = replacement(actorRef);
        fixture.runtime()
                .handleBoundSessionReplaced(
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
        assertEquals(0, session.replacementCallbacks.get());

        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), command);
        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), command);
        fixture.runtime()
                .handleBoundSessionReplaced(
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
        assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
        ZLinkActor actor =
                fixture.actors()
                        .getOrCreateLocalActor(REPLACEMENT_ACTOR, ZLinkActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        ZLinkBackendActorRef actorRef = fixture.actors().currentRef(actor);
        session.context()
                .actors()
                .bind(
                        new ActorRef(
                                actorRef.actorId(),
                                actorRef.generation(),
                                MESH,
                                actorRef.nodeRid()))
                .toCompletableFuture()
                .join();

        long started = System.nanoTime();
        fixture.runtime().handleBoundSessionReplaced(actorRef.nodeRid(), replacement(actorRef));

        assertTrue(session.replacementEntered.await(5, TimeUnit.SECONDS));
        assertEquals(PEER_A, stream.disconnectedPeer.get(5, TimeUnit.SECONDS));
        assertEquals(1, session.replacementCallbacks.get());
        assertFalse(session.replacementCompletion.isDone());
        assertTrue(
                stream.disconnectStartedAt - started >= TimeUnit.MILLISECONDS.toNanos(100),
                "a stalled callback must close after its deadline");
    }

    private static ReplacementFixture startReplacement(FakeStream stream) {
        stream.enqueue(PEER_A, frame("initial", "{}"));
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setSessionReplacementCallbackTimeout(Duration.ofMillis(100));
        options.addStreamNode("stream")
                .bind("tcp://127.0.0.1:18081")
                .registerSession(TestSession.class);
        ZLinkFrameworkRegistration registration = options.registration();
        ZLinkJsonMessageSerializer serializer = new ZLinkJsonMessageSerializer();
        ZLinkInternalSpotNode spotNode =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
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
        ZLinkActorRuntime actors =
                new ZLinkActorRuntime(
                        spotNode,
                        Map.of("probe", ProbeFactory.class),
                        Duration.ofSeconds(1),
                        serializer,
                        ZLinkHandlerActivator.reflection());
        actors.setMeshName(MESH);
        ZLinkInternalMeshNode ownerNode =
                (ZLinkInternalMeshNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalMeshNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalMeshNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "routingId" ->
                                                    RoutingId.from("session-owner-node");
                                            case "lifecycleGeneration" -> 3L;
                                            case "localAuthorityOwnerId" -> "session-owner";
                                            case "localAuthorityLeaseGeneration" -> 5L;
                                            default -> defaultValue(method.getReturnType());
                                        });
        ZLinkStreamRuntime runtime =
                new ZLinkStreamRuntime(
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
                                false)
                        .start();
        return new ReplacementFixture(runtime, actors);
    }

    private static systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                    .BoundSessionReplaced
            replacement(ZLinkBackendActorRef actorRef) {
        return new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                .BoundSessionReplaced(
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                        .ActorRouteFence(actorRef, 3, 7, 11),
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                        .RetiredSessionRouteFence(
                        RoutingId.from("session-owner-node"), 3, "session-owner", 5, PEER_A, 1));
    }

    private record ReplacementFixture(ZLinkStreamRuntime runtime, ZLinkActorRuntime actors) {}

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
        return start(stream, hwm, maxMessageSize, ignored -> {});
    }

    private ZLinkStreamRuntime start(
            FakeStream stream,
            long hwm,
            long maxMessageSize,
            java.util.function.Consumer<ZLinkFrameworkRegistration> configure) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkFrameworkRegistration registration = streamRegistration(options, maxMessageSize);
        configure.accept(registration);
        return start(stream, registration);
    }

    private ZLinkFrameworkRegistration streamRegistration(
            DefaultZLinkFrameworkOptions options, long maxMessageSize) {
        var streamNode =
                options.addStreamNode("stream")
                        .bind("tcp://127.0.0.1:18081")
                        .registerSession(TestSession.class);
        streamNode.configureSocket().setMaxMessageSize(maxMessageSize);
        return options.registration();
    }

    private ZLinkStreamRuntime start(FakeStream stream, ZLinkFrameworkRegistration registration) {
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
                                        CompletableFuture.completedFuture(null))
                .start();
    }

    private ZLinkStreamRuntime startWithCustomReceiveCodec(FakeStream stream) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("stream")
                .bind("tcp://127.0.0.1:18081")
                .registerSession(TestSession.class);
        ZLinkFrameworkRegistration registration = options.registration();
        ZLinkCodecRegistration codecs = registration.codecs();
        codecs.addSerializer(
                "application/x-wire", new WirePayloadSerializer(), WirePayload.class::equals);
        codecs.addStreamCodec("application/x-wire", ZLinkStreamCodec.PROTOBUF);
        codecs.freeze();
        ZLinkMessageSerializer serializer =
                codecs.serializerWithFallback(new ZLinkJsonMessageSerializer());
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
                                        CompletableFuture.completedFuture(null))
                .start();
    }

    private void assertLivenessCloseReason(FakeStream stream, String expectedReason)
            throws Exception {
        TestSession.lastSession.set(null);
        stream.enqueue(PEER_A, frame("initial", "{}"));
        List<String> closeReasons = Collections.synchronizedList(new ArrayList<>());
        try (AutoCloseable ignored = installClosedMetricSink(closeReasons)) {
            ZLinkStreamRuntime runtime = start(stream, 0);
            runtimes.add(runtime);

            TestSession session = awaitSession();
            assertTrue(session.dispatchLatch.await(5, TimeUnit.SECONDS));
            assertTrue(stream.sessionClosingSendsLatch.await(35, TimeUnit.SECONDS));
            runtime.closeAsync().toCompletableFuture().join();

            assertEquals(List.of(expectedReason), closeReasons);
        }
    }

    private static AutoCloseable installClosedMetricSink(List<String> closeReasons) {
        return ZLinkRuntimeMetrics.install(
                new ZLinkRuntimeMetrics.Sink() {
                    @Override
                    public void increment(String name, Map<String, String> tags) {
                        if ("zlink.stream.connections.closed".equals(name)) {
                            closeReasons.add(tags.get("close_reason"));
                        }
                    }
                });
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

    private static void awaitValue(AtomicInteger value, int expected) throws Exception {
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

    private static byte[] frame(String packetName, ZLinkStreamCodec codec, String payload) {
        ZLinkStreamHeader header =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.SEND,
                        codec,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.empty(),
                        packetName,
                        Map.of());
        return ZLinkStreamFrameCodec.encode(
                ZLinkStreamHeaderCodec.encode(header), payload.getBytes(StandardCharsets.UTF_8));
    }

    private static byte[] controlFrame(String packetName) {
        ZLinkStreamHeader header =
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.CONTROL,
                        ZLinkStreamCodec.RAW,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.empty(),
                        packetName,
                        Map.of());
        return ZLinkStreamFrameCodec.encode(ZLinkStreamHeaderCodec.encode(header), new byte[0]);
    }

    private static void awaitCondition(java.util.function.BooleanSupplier condition)
            throws Exception {
        awaitCondition(condition, "condition was not reached");
    }

    private static void awaitCondition(java.util.function.BooleanSupplier condition, String message)
            throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertTrue(condition.getAsBoolean(), message);
    }

    /** Whether any live thread currently executes code of the named class. */
    private static boolean anyThreadRuns(String className) {
        return Thread.getAllStackTraces().values().stream()
                .flatMap(Arrays::stream)
                .anyMatch(frame -> frame.getClassName().equals(className));
    }

    public static final class TestSession implements ZLinkSession {
        private static final AtomicReference<TestSession> lastSession = new AtomicReference<>();
        private static CompletableFuture<TestSession> connected = new CompletableFuture<>();
        private final List<String> transportCallbacks = new ArrayList<>();
        private final CompletableFuture<List<String>> transportClosed = new CompletableFuture<>();
        private static final AtomicInteger createdCount = new AtomicInteger();
        private static volatile boolean holdFirstDispatch;
        private static volatile boolean failNextConstruction;
        private static volatile Runnable constructionHook;
        private static volatile ReplacementMode replacementMode = ReplacementMode.NONE;
        private static volatile boolean decodeWirePayload;
        private static volatile boolean replyOnDispatch;
        private static final List<TestSession> created = new CopyOnWriteArrayList<>();
        private static final AtomicReference<WirePayload> decodedWirePayload =
                new AtomicReference<>();
        private final ZLinkSessionContext context;
        private final CountDownLatch dispatchLatch = new CountDownLatch(1);
        private final CountDownLatch secondDispatchLatch = new CountDownLatch(1);
        private final CompletableFuture<Void> firstDispatch = new CompletableFuture<>();
        private final AtomicInteger dispatchCount = new AtomicInteger();
        private final AtomicInteger replacementCallbacks = new AtomicInteger();
        private final CountDownLatch replacementEntered = new CountDownLatch(1);
        private final CompletableFuture<Long> replacementCompletion = new CompletableFuture<>();
        private final List<String> packetNames = Collections.synchronizedList(new ArrayList<>());

        public TestSession(ZLinkSessionContext context) {
            if (constructionHook != null) {
                constructionHook.run();
            }
            if (failNextConstruction) {
                failNextConstruction = false;
                throw new IllegalStateException("test session construction failure");
            }
            this.context = context;
            created.add(this);
            createdCount.incrementAndGet();
            lastSession.set(this);
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            connected.complete(this);
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            transportCallbacks.add("disconnected");
            transportClosed.complete(List.copyOf(transportCallbacks));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            transportCallbacks.add("error:" + error.message());
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onActorBindingReplaced(String actorId) {
            replacementCallbacks.incrementAndGet();
            replacementEntered.countDown();
            return switch (replacementMode) {
                case SUCCESS -> {
                    replacementCompletion.complete(System.nanoTime());
                    yield CompletableFuture.completedFuture(null);
                }
                case NONE -> CompletableFuture.completedFuture(null);
                case FAILURE ->
                        CompletableFuture.failedFuture(
                                new IllegalStateException("replacement callback failure"));
                case PENDING -> replacementCompletion.thenApply(ignored -> null);
            };
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if (decodeWirePayload) {
                decodedWirePayload.set(payload.decode(WirePayload.class));
            }
            int count = dispatchCount.incrementAndGet();
            packetNames.add(dispatch.packetName());
            dispatchLatch.countDown();
            if (replyOnDispatch && dispatch.canReply()) {
                return context.client().reply(Map.of("ok", true)).submit();
            }
            if (count == 1 && holdFirstDispatch) {
                return firstDispatch;
            }
            secondDispatchLatch.countDown();
            return CompletableFuture.completedFuture(null);
        }
    }

    private record WirePayload(String marker) {}

    private static final class WirePayloadSerializer implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            return ZLinkEncodedPayload.from("CUSTOM".getBytes(StandardCharsets.UTF_8));
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

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {}

    private static final class FakeProvider implements ZLinkBackendAdapterProvider {
        private final FakeStream stream;

        private FakeProvider(FakeStream stream) {
            this.stream = stream;
        }

        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            return (context, meshNode) -> stream;
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class FakeContext implements ZLinkBackendContext {
        @Override
        public String name() {
            return "fake-context";
        }

        @Override
        public void close() {}

        @Override
        public void shutdown() {}
    }

    private static class FakeStream implements ZLinkBackendStreamSocket {
        private Runnable readabilityObserver = () -> {};
        private final Queue<ZLinkBackendStreamReceived> received = new ConcurrentLinkedQueue<>();
        private final AtomicInteger successfulReceives = new AtomicInteger();
        private final AtomicInteger readinessWaits = new AtomicInteger();
        private final AtomicInteger zeroReadinessWaits = new AtomicInteger();
        private final AtomicBoolean receiveReady = new AtomicBoolean();
        private final AtomicInteger sessionClosingSends = new AtomicInteger();
        private final CountDownLatch sessionClosingSendsLatch = new CountDownLatch(1);
        private final CompletableFuture<RoutingId> disconnectedPeer = new CompletableFuture<>();
        private final AtomicInteger disconnectCalls = new AtomicInteger();
        private volatile long disconnectStartedAt;
        private boolean completeClosingControlSend = true;
        private final CompletableFuture<Void> closingControlCompletion = new CompletableFuture<>();
        private final CountDownLatch firstReceiveEntered = new CountDownLatch(1);
        private final CountDownLatch firstReceiveRelease = new CountDownLatch(1);
        private volatile boolean blockFirstReceive;
        private volatile boolean ignoreFirstReceiveInterrupt;
        private volatile boolean failHeartbeatPongSend;
        private volatile boolean failHeartbeatPingSend;
        private volatile boolean deferHeartbeatPongSend;
        private final CountDownLatch heartbeatPingAttempted = new CountDownLatch(2);
        private final CountDownLatch heartbeatPongAsyncAttempted = new CountDownLatch(1);
        private final AtomicInteger synchronousHeartbeatPongSends = new AtomicInteger();
        private final CompletableFuture<Void> deferredHeartbeatPong = new CompletableFuture<>();
        private final AtomicInteger closeCalls = new AtomicInteger();
        private ZLinkBackendStreamErrorHandler errorHandler;

        private void enqueue(RoutingId routingId, byte[] bytes) {
            enqueuePacket(routingId, bytes, null);
        }

        private void enqueueTracked(RoutingId routingId, byte[] bytes, AtomicInteger ownerCloses) {
            enqueuePacket(routingId, bytes, ownerCloses);
        }

        private void enqueueEmptyParts(RoutingId routingId) {
            Message header = Message.from(new byte[0]);
            Message body = Message.from(new byte[0]);
            received.add(
                    new ZLinkBackendStreamReceived(
                            Optional.of(routingId),
                            header,
                            body,
                            () -> {
                                header.close();
                                body.close();
                            }));
        }

        private void enqueuePacket(RoutingId routingId, byte[] bytes, AtomicInteger ownerCloses) {
            ByteBuffer encoded = ByteBuffer.wrap(bytes);
            int headerSize =
                    bytes.length >= 6 ? Short.toUnsignedInt(encoded.getShort()) : bytes.length;
            int bodySize = bytes.length >= 6 ? encoded.getInt() : 0;
            boolean complete =
                    headerSize >= 0 && bodySize >= 0 && bytes.length == 6L + headerSize + bodySize;
            byte[] headerBytes;
            byte[] bodyBytes;
            if (complete) {
                headerBytes = new byte[headerSize];
                bodyBytes = new byte[bodySize];
                encoded.get(headerBytes);
                encoded.get(bodyBytes);
            } else {
                headerBytes = bytes.clone();
                bodyBytes = new byte[0];
            }
            Message header = Message.from(headerBytes);
            Message body = Message.from(bodyBytes);
            received.add(
                    new ZLinkBackendStreamReceived(
                            Optional.of(routingId),
                            header,
                            body,
                            () -> {
                                header.close();
                                body.close();
                                if (ownerCloses != null) {
                                    ownerCloses.incrementAndGet();
                                }
                            }));
        }

        private void blockFirstReceive() {
            blockFirstReceive = true;
        }

        private void blockFirstReceiveIgnoringInterrupt() {
            blockFirstReceive = true;
            ignoreFirstReceiveInterrupt = true;
        }

        @Override
        public String name() {
            return "fake-stream";
        }

        @Override
        public void close() {
            closeCalls.incrementAndGet();
        }

        @Override
        public void bind(String endpoint) {}

        @Override
        public void setTlsServer(
                String certificatePath, String keyPath, boolean requireClientCertificate) {}

        @Override
        public void setMaxMessageSize(long value) {}

        @Override
        public boolean waitForReadable(Duration timeout) {
            Runnable observer = readabilityObserver;
            readabilityObserver = () -> {};
            observer.run();
            readinessWaits.incrementAndGet();
            if (timeout.isZero()) {
                zeroReadinessWaits.incrementAndGet();
            }
            boolean readable = !received.isEmpty();
            receiveReady.set(readable);
            return readable;
        }

        @Override
        public ZLinkBackendStreamReceived recv() {
            if (!receiveReady.get()) {
                throw new AssertionError("STREAM recv was called before readiness");
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
            if (next == null) {
                receiveReady.set(false);
            }
            if (next != null) {
                successfulReceives.incrementAndGet();
            }
            return next;
        }

        @Override
        public void disconnectPeer(RoutingId routingId) {
            disconnectStartedAt = System.nanoTime();
            disconnectCalls.incrementAndGet();
            disconnectedPeer.complete(routingId);
        }

        @Override
        public void onTransportError(ZLinkBackendStreamErrorHandler handler) {
            errorHandler = handler;
        }

        @Override
        public void startSessionService() {}

        @Override
        public boolean send(RoutingId routingId, List<Message> parts, SendFlags flags) {
            return true;
        }

        @Override
        public boolean send(
                RoutingId routingId, String packetName, List<Message> parts, SendFlags flags) {
            return true;
        }

        @Override
        public boolean send(
                RoutingId routingId,
                ZLinkStreamHeader header,
                List<Message> parts,
                SendFlags flags) {
            if (HEARTBEAT_PING.equals(header.packetName())) {
                heartbeatPingAttempted.countDown();
                if (failHeartbeatPingSend) {
                    throw new ZlinkSubmitException(SubmitResult.NOT_CONNECTED);
                }
            }
            if (failHeartbeatPongSend && HEARTBEAT_PONG.equals(header.packetName())) {
                return false;
            }
            if (deferHeartbeatPongSend && HEARTBEAT_PONG.equals(header.packetName())) {
                synchronousHeartbeatPongSends.incrementAndGet();
                return true;
            }
            if (ZLinkSessionClosingControl.NAME.equals(header.packetName())) {
                sessionClosingSends.incrementAndGet();
                sessionClosingSendsLatch.countDown();
            }
            return true;
        }

        @Override
        public CompletionStage<Void> sendAsync(
                RoutingId routingId, ZLinkStreamHeader header, List<Message> parts) {
            if (ZLinkSessionClosingControl.NAME.equals(header.packetName())) {
                sessionClosingSends.incrementAndGet();
                sessionClosingSendsLatch.countDown();
                return completeClosingControlSend
                        ? CompletableFuture.completedFuture(null)
                        : closingControlCompletion;
            }
            if (deferHeartbeatPongSend && HEARTBEAT_PONG.equals(header.packetName())) {
                heartbeatPongAsyncAttempted.countDown();
                return deferredHeartbeatPong;
            }
            return ZLinkBackendStreamSocket.super.sendAsync(routingId, header, parts);
        }

        @Override
        public boolean reply(
                RoutingId routingId,
                long requestSeq,
                String packetName,
                List<Message> parts,
                SendFlags flags) {
            return true;
        }

        @Override
        public boolean reply(
                RoutingId routingId,
                ZLinkStreamHeader header,
                List<Message> parts,
                SendFlags flags) {
            return true;
        }

        @Override
        public ZLinkBackendActorBindOperation bindActor(
                RoutingId sessionRid, ZLinkBackendActorRef actor) {
            return timeout -> CompletableFuture.completedFuture(null);
        }

        @Override
        public ZLinkBackendActorUnbindOperation unbindActor(RoutingId sessionRid, String actorId) {
            return timeout -> CompletableFuture.completedFuture(null);
        }

        @Override
        public boolean sendBoundActor(
                RoutingId sessionRid, String actorId, List<Message> parts, SendFlags flags) {
            return true;
        }

        @Override
        public boolean relayBoundActor(
                RoutingId sessionRid,
                String actorId,
                ZLinkStreamHeader header,
                List<Message> parts,
                SendFlags flags) {
            return true;
        }
    }
}
