package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamRuntimeIngressTest {
    private static final RoutingId PEER_A = RoutingId.from("peer-a");
    private static final RoutingId PEER_B = RoutingId.from("peer-b");
    private final List<ZLinkStreamRuntime> runtimes = new ArrayList<>();
    private ZLinkFrameworkRegistration lastRegistration;

    @AfterEach
    void resetSessionProbe() {
        TestSession.holdFirstDispatch = false;
        TestSession.failNextConstruction = false;
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
        stream.enqueue(PEER_A, java.util.Arrays.copyOfRange(frame, 0, 4));
        stream.enqueue(PEER_A, java.util.Arrays.copyOfRange(frame, 4, frame.length));

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
    void continuesReceivingButSerializesSessionDispatchAtApplicationHwm()
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
        assertEquals(
            0,
            lastRegistration.inboundDispatchBudget().snapshot().pendingPayloadBytes());
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

    private ZLinkStreamRuntime start(FakeStream stream, long hwm) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addStreamNode("stream")
            .bind("tcp://127.0.0.1:18081")
            .registerSession(TestSession.class);
        options.configureInboundDispatch().setApplicationHwmBytes(hwm);
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

    private static byte[] frame(String packetName, String payload) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            java.util.EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName,
            Map.of());
        return ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header),
            payload.getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    private static byte[] controlFrame(String packetName) {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.CONTROL,
            ZLinkStreamCodec.RAW,
            java.util.EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
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
        private final ZLinkSessionContext context;
        private final CountDownLatch dispatchLatch = new CountDownLatch(1);
        private final CountDownLatch secondDispatchLatch = new CountDownLatch(1);
        private final CompletableFuture<Void> firstDispatch = new CompletableFuture<>();
        private final AtomicInteger dispatchCount = new AtomicInteger();
        private final List<String> packetNames =
            java.util.Collections.synchronizedList(new ArrayList<>());

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
        @Override public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            systems.zlink.framework.messaging.ZLinkMessage payload) {
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
            }
            return true;
        }
        @Override public boolean reply(
            RoutingId routingId, long requestSeq, String packetName,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public boolean reply(
            RoutingId routingId, ZLinkStreamHeader header,
            List<Message> parts, SendFlags flags) { return true; }
        @Override public systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation
            bindActor(RoutingId sessionRid,
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor) {
            return timeout -> CompletableFuture.completedFuture(null);
        }
        @Override public systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation
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
