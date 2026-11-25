package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.*;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;
import systems.zlink.stream.connector.ZLinkStreamDispatchMode;

import java.io.IOException;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.URI;
import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.FileHandler;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import java.util.logging.SimpleFormatter;

final class ZLinkAcceptedSessionSendCloseTest {
    private static final String LATE_PACKET = "LateSend";
    private static final Duration TIMEOUT = Duration.ofSeconds(5);
    private static final CompletionStage<Void> DONE = CompletableFuture.completedFuture(null);
    private static final Logger FLOW =
            Logger.getLogger("systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
    private static FileHandler flowFile;
    private static Observations observations;

    @BeforeAll
    static void captureFlow() throws IOException {
        String logPath = System.getenv("ZLINK_TEST_ACCEPTED_SEND_CLOSE_FLOW");
        if (logPath != null) {
            flowFile = new FileHandler(logPath);
            flowFile.setFormatter(new SimpleFormatter());
            FLOW.addHandler(flowFile);
        }
    }

    @AfterAll
    static void closeFlow() {
        if (flowFile != null) {
            FLOW.removeHandler(flowFile);
            flowFile.close();
        }
    }

    @RepeatedTest(40)
    void acceptedSendReachesActorAfterTransportClose() throws Exception {
        runSend(true, false, false);
    }

    @Test
    void sendReachesActorBeforeTransportClose() throws Exception {
        runSend(false, false, false);
    }

    @Test
    void framePulledAfterTransportCloseIsRejected() throws Exception {
        runSend(false, true, false);
    }

    @Test
    void deadlineNotifiesActorAndDropsLateRelay() throws Exception {
        runSend(true, false, true);
    }

    private static void runSend(boolean closeBeforeRelay, boolean injectAfterClose, boolean expire)
            throws Exception {
        var observed = new Observations();
        observations = observed;
        var backend = new DisconnectObserverBackend();
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
        var mesh = options.addRouteMesh("game");
        mesh.listen("inproc://accepted-send-" + System.nanoTime());
        mesh.objects()
                .server()
                .addEntrySpot(SessionActorsRuntimeIntegrationTest.GameEntrySpot.class)
                .addActorFactory(
                        "player",
                        SessionActorsRuntimeIntegrationTest.PlayerActor.class,
                        SessionActorsRuntimeIntegrationTest.PlayerActorFactory.class,
                        factory -> factory.disableRelocation());
        var stream = options.addStreamNode("gateway");
        stream.bind("tcp://127.0.0.1:0");
        stream.enableActorDispatch();
        stream.registerSession(RelaySession.class);

        var received = SessionActorsRuntimeIntegrationTest.actorRelayRequests;
        received.clear();
        String id = SessionActorsRuntimeIntegrationTest.uniqueActorId("accepted-send");
        try (var runtime = RuntimeTestSupport.startFixedRidFramework(options, backend)) {
            var created =
                    assertInstanceOf(
                            ZLinkActorCreateResult.Created.class,
                            await(runtime.actorManager().create(id, "player").submit()));
            var actor =
                    (SessionActorsRuntimeIntegrationTest.PlayerActor)
                            await(
                                    ZLinkFrameworkRuntimeTestAccess.actors(runtime)
                                            .getOrCreateManagedActor(id, "player"));
            var actorDisconnected = new CompletableFuture<Void>();
            actor.disconnectCompletion = actorDisconnected;
            var relayDropped = new CompletableFuture<Void>();
            var dropListener =
                    new Handler() {
                        @Override
                        public void publish(LogRecord record) {
                            String message = record.getMessage();
                            if (message.contains("outcome=dropped")
                                    && message.contains("actor=" + id)
                                    && message.contains("packet=ActorNotify")) {
                                relayDropped.complete(null);
                            }
                        }

                        @Override
                        public void flush() {}

                        @Override
                        public void close() {}
                    };

            var endpoint =
                    URI.create(
                            runtime.listenerStatus(ZLinkListenerKind.STREAM, "gateway").endpoint());
            var connector =
                    ZLinkStreamConnectorFactory.create(
                            new ZLinkStreamConnectorOptions(
                                    endpoint,
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    Duration.ofSeconds(30),
                                    3,
                                    TIMEOUT,
                                    65536,
                                    false,
                                    Duration.ofSeconds(1),
                                    TIMEOUT,
                                    false,
                                    Duration.ofMillis(250),
                                    TIMEOUT,
                                    2.0));
            var boundOnClient = new CompletableFuture<String>();
            connector.onActorBound(
                    bound -> {
                        boundOnClient.complete(bound.actorId());
                        return DONE;
                    });

            FLOW.addHandler(dropListener);
            try {
                await(connector.connect().submit());
                var context = await(observed.connected);
                await(context.actors().bindOrGet(created.actor()));
                assertEquals(id, await(boundOnClient));
                await(
                        connector
                                .send(
                                        "ActorNotify",
                                        new SessionActorsRuntimeIntegrationTest.ActorNotifyMessage(
                                                id))
                                .submit());
                await(observed.handlerEntered);
                if (closeBeforeRelay) {
                    await(connector.close().submit());
                    await(backend.disconnected);
                    assertFalse(observed.sessionClosed.isDone());
                }
                if (expire) {
                    assertThrows(
                            TimeoutException.class,
                            () -> actor.disconnected.get(250, TimeUnit.MILLISECONDS));
                    actor.disconnected.get(8, TimeUnit.SECONDS);
                    assertFalse(observed.sessionClosed.isDone());
                }
                observed.releaseHandler.complete(null);
                if (expire) {
                    var failed =
                            assertThrows(ExecutionException.class, () -> await(observed.relayed));
                    assertEquals(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            assertInstanceOf(ZLinkFrameworkException.class, failed.getCause())
                                    .kind());
                    await(relayDropped);
                } else {
                    await(observed.relayed);
                    assertEquals(
                            id + ":" + id,
                            received.poll(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS));
                }
                if (!closeBeforeRelay) {
                    await(connector.close().submit());
                }
                if (injectAfterClose) {
                    await(backend.injectLatePacket(await(backend.disconnected)));
                    assertEquals(0, observed.lateDispatchCount.get());
                    assertEquals(1, observed.connectedCount.get());
                }
                actor.disconnectCompletion.complete(null);
                await(observed.sessionClosed);
                assertEquals(1, actor.disconnectCount.get());
                assertNull(received.poll());
            } finally {
                FLOW.removeHandler(dropListener);
                observed.releaseHandler.complete(null);
                actor.disconnectCompletion.complete(null);
                await(connector.close().submit());
            }
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(TIMEOUT.toMillis(), TimeUnit.MILLISECONDS);
    }

    private static final class Observations {
        final CompletableFuture<ZLinkSessionContext> connected = new CompletableFuture<>();
        final CompletableFuture<Void> handlerEntered = new CompletableFuture<>();
        final CompletableFuture<Void> releaseHandler = new CompletableFuture<>();
        final CompletableFuture<Void> relayed = new CompletableFuture<>();
        final AtomicInteger connectedCount = new AtomicInteger();
        final AtomicInteger lateDispatchCount = new AtomicInteger();
        final CompletableFuture<Void> sessionClosed = new CompletableFuture<>();
    }

    public static final class RelaySession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final Observations observed = observations;

        public RelaySession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            observed.connectedCount.incrementAndGet();
            observed.connected.complete(context);
            return DONE;
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            observed.sessionClosed.complete(null);
            return DONE;
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return DONE;
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if (LATE_PACKET.equals(dispatch.packetName())) {
                observed.lateDispatchCount.incrementAndGet();
                return DONE;
            }
            var actor = context.actors().bound().getFirst();
            observed.handlerEntered.complete(null);
            return observed.releaseHandler
                    .thenCompose(ignored -> actor.relay(dispatch, payload))
                    .whenComplete(
                            (ignored, error) -> {
                                if (error == null) {
                                    observed.relayed.complete(null);
                                } else {
                                    observed.relayed.completeExceptionally(error);
                                }
                            });
        }
    }

    private static final class DisconnectObserverBackend implements ZLinkBackendAdapterProvider {
        private final CompletableFuture<RoutingId> disconnected = new CompletableFuture<>();
        private final AtomicReference<ZLinkBackendStreamReceived> latePacket =
                new AtomicReference<>();

        CompletionStage<Void> injectLatePacket(RoutingId peer) {
            var header =
                    new ZLinkStreamHeader(
                            ZLinkStreamMessageKind.SEND,
                            ZLinkStreamCodec.RAW,
                            null,
                            Optional.empty(),
                            LATE_PACKET,
                            Map.of());
            var headerFrame = Message.from(ZLinkStreamHeaderCodec.encode(header));
            var body = Message.allocate(0);
            var closed = new CompletableFuture<Void>();
            latePacket.set(
                    new ZLinkBackendStreamReceived(
                            Optional.of(peer),
                            headerFrame,
                            body,
                            () -> {
                                headerFrame.close();
                                body.close();
                                closed.complete(null);
                            }));
            return closed;
        }

        private static final ZLinkBackendAdapterProvider DELEGATE =
                new ZLinkJavaBackendAdapterFactory();

        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
            return DELEGATE.createChannelAdapter(options);
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
            return DELEGATE.createSpotAdapter(options);
        }

        @Override
        public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
            return DELEGATE.createMeshAdapter(options);
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                ZLinkBackendAdapterOptions options) {
            return DELEGATE.createMonitoringAdapter(options);
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
            var adapter = DELEGATE.createStreamAdapter(options);
            return (context, mesh) -> {
                var socket = adapter.createStreamSocket(context, mesh);
                return (ZLinkBackendStreamSocket)
                        Proxy.newProxyInstance(
                                ZLinkBackendStreamSocket.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendStreamSocket.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("onTransportError")) {
                                        var handler = (ZLinkBackendStreamErrorHandler) arguments[0];
                                        socket.onTransportError(
                                                (rid, event, code, message) -> {
                                                    handler.handle(rid, event, code, message);
                                                    if (event == MonitorEventType.DISCONNECTED) {
                                                        disconnected.complete(rid);
                                                    }
                                                });
                                        return null;
                                    }
                                    if (method.getName().equals("waitForReadable")
                                            && latePacket.get() != null) {
                                        return true;
                                    }
                                    if (method.getName().equals("recv")) {
                                        var injected = latePacket.getAndSet(null);
                                        if (injected != null) {
                                            return injected;
                                        }
                                    }
                                    try {
                                        return method.invoke(socket, arguments);
                                    } catch (InvocationTargetException failure) {
                                        throw failure.getCause();
                                    }
                                });
            };
        }
    }
}
