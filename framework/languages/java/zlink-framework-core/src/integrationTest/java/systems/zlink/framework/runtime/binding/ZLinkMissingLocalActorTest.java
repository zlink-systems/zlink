package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.*;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.*;
import systems.zlink.framework.spots.*;
import systems.zlink.framework.streams.*;
import systems.zlink.framework.testsupport.ZLinkStreamTestFrames;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.LongSupplier;
import java.util.logging.FileHandler;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import java.util.logging.SimpleFormatter;

final class ZLinkMissingLocalActorTest {
    private static final String ACTOR = "missing-local-actor";
    private static final LinkedBlockingQueue<ProbeActor> created = new LinkedBlockingQueue<>();
    private static final LinkedBlockingQueue<RoutingId> connected = new LinkedBlockingQueue<>();
    private static final Logger flowLogger =
            Logger.getLogger("systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer");
    private static final LinkedBlockingQueue<String> replies = new LinkedBlockingQueue<>();
    private static FileHandler flowLog;
    private static final Handler replyLog =
            new Handler() {
                public void publish(LogRecord record) {
                    if (record.getMessage().contains("phase=replied")
                            && record.getMessage().contains("actor=" + ACTOR))
                        replies.offer(record.getMessage());
                }

                public void flush() {}

                public void close() {}
            };

    @BeforeAll
    static void captureFlow() throws Exception {
        flowLog = new FileHandler("build/missing-local-actor.flow");
        flowLog.setFormatter(new SimpleFormatter());
        flowLogger.addHandler(flowLog);
        flowLogger.addHandler(replyLog);
    }

    @AfterAll
    static void closeFlow() {
        flowLogger.removeHandler(flowLog);
        flowLogger.removeHandler(replyLog);
        flowLog.close();
    }

    @RepeatedTest(40)
    void requestToMissingSameNodeActorIsNotFoundAndEvictsItsRoute() throws Exception {
        try (var fixture = new Fixture()) {
            assertEquals("warm", fixture.request("warm").get(3, TimeUnit.SECONDS));
            int reads = fixture.authorityReads.get();
            var stay = fixture.spots.actors.remove(ACTOR);
            assertNotNull(stay);
            try {
                assertNotFound(fixture.request("missing"));
                assertEquals(reads, fixture.authorityReads.get(), "first request uses warm route");
                assertNotFound(fixture.request("missing again"));
                assertEquals(
                        reads + 1, fixture.authorityReads.get(), "next request resolves again");
            } finally {
                fixture.spots.actors.put(ACTOR, stay);
            }
            assertEquals("restored", fixture.request("restored").get(3, TimeUnit.SECONDS));
        }
    }

    @RepeatedTest(40)
    void boundSessionRequestToDepartedActorCarriesNotFoundToClient() throws Exception {
        try (var fixture = new Fixture();
                var client = fixture.connect()) {
            var connectedSession = connected.poll(3, TimeUnit.SECONDS);
            assertNotNull(connectedSession);
            // Exercise a native binding without the managed-Actor dispatch shortcut.
            var session =
                    new ZLinkSessionActorsRuntime(
                            fixture.spots,
                            fixture.stream,
                            connectedSession,
                            null,
                            new systems.zlink.framework.runtime.messaging
                                    .ZLinkJsonMessageSerializer(),
                            null,
                            true,
                            ZLinkStreamCodec.JSON);
            // Queue a ping before binding to exercise the wire order deterministically.
            fixture.sendHeartbeatPing(connectedSession);
            session.bindOrGet(fixture.ref).toCompletableFuture().get(3, TimeUnit.SECONDS);
            var control = ZLinkStreamTestFrames.readSkippingHeartbeatPings(client);
            assertEquals(ZLinkStreamActorControl.BOUND, control.header().name());
            var controlBody = ByteBuffer.wrap(control.body());
            assertEquals(1, controlBody.get());
            int slot = Short.toUnsignedInt(controlBody.getShort());
            var stay = fixture.spots.actors.remove(ACTOR);
            assertNotNull(stay);
            try {
                // The Actor stay can retire before the Session owner removes its binding.
                var header =
                        new ZLinkStreamHeader(
                                        ZLinkStreamMessageKind.REQUEST,
                                        ZLinkStreamCodec.JSON,
                                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                        Optional.of(41L),
                                        "Echo",
                                        Map.of())
                                .withActorSlot(slot);
                fixture.sendHeartbeatPing(connectedSession);
                // Inject backend ingress after Actor retirement while the published binding
                // survives.
                try (var payload = Message.from("{\"value\":\"missing\"}")) {
                    assertTrue(
                            fixture.stream.relayBoundActor(
                                    connectedSession,
                                    ACTOR,
                                    header,
                                    List.of(payload),
                                    SendFlags.DONT_WAIT));
                }
                var frame = ZLinkStreamTestFrames.readSkippingHeartbeatPings(client);
                var reply = frame.header();
                var error = ZLinkStreamErrorPayload.decode(frame.body());
                assertEquals(ZLinkStreamMessageKind.ERROR, reply.kind());
                assertEquals(Optional.of(41L), reply.requestSequence());
                CompletableFuture<Void> relayed;
                session.enterRelayDispatch(header);
                try {
                    relayed =
                            session.find(ACTOR)
                                    .orElseThrow()
                                    .relay(ZLinkMessage.of(new Echo("missing")))
                                    .toCompletableFuture();
                } finally {
                    session.exitRelayDispatch();
                }
                assertAll(
                        () ->
                                assertEquals(
                                        ZLinkFrameworkErrorKind.NOT_FOUND, error.frameworkKind()),
                        () -> assertNotFound(relayed));
            } finally {
                fixture.spots.actors.put(ACTOR, stay);
            }
        }
    }

    @RepeatedTest(40)
    void replyAfterTimeoutAndActorDepartureDoesNotFailDispatch() throws Exception {
        var requestTimeout = Duration.ofMillis(100);
        var now = new AtomicLong();
        try (var fixture = new Fixture(now::get)) {
            fixture.actor.reply = new CompletableFuture<>();
            replies.clear();
            var request =
                    fixture.runtime
                            .actorClient()
                            .requestToActor(ACTOR, new Echo("late"))
                            .timeout(requestTimeout)
                            .submit(String.class)
                            .toCompletableFuture();
            fixture.actor.entered.get(3, TimeUnit.SECONDS);
            assertFalse(request.isDone(), "request remains pending before its deadline");
            // Advance the request clock only after the Actor has entered its handler.
            now.addAndGet(requestTimeout.toNanos());
            var timeout =
                    assertThrows(ExecutionException.class, () -> request.get(3, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    assertInstanceOf(ZLinkFrameworkException.class, timeout.getCause()).kind());
            var stay = fixture.spots.actors.remove(ACTOR);
            assertNotNull(stay);
            try {
                fixture.actor.reply.complete("late");
                assertNotNull(
                        replies.poll(3, TimeUnit.SECONDS),
                        "late reply must finish without an exception");
                assertSame(
                        timeout.getCause(),
                        assertThrows(ExecutionException.class, () -> request.get()).getCause());
            } finally {
                fixture.spots.actors.put(ACTOR, stay);
            }
        }
    }

    private static void assertNotFound(CompletableFuture<?> result) {
        var failure = assertThrows(ExecutionException.class, () -> result.get(3, TimeUnit.SECONDS));
        assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
    }

    private static final class Fixture implements AutoCloseable {
        final AtomicInteger authorityReads = new AtomicInteger();
        final ZLinkFrameworkRuntime runtime;
        final ZLinkJavaRawSpotNode spots;
        final ZLinkBackendStreamSocket stream;
        final ProbeActor actor;
        final ActorRef ref;

        Fixture() throws Exception {
            this(System::nanoTime);
        }

        Fixture(LongSupplier nanoTime) throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            var store = new ZLinkInMemoryLocationStore();
            options.addLocationStore(
                    new ZLinkLocationStore() {
                        public CompletionStage<ZLinkStoreReadResult> read(
                                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                            if (key.value().equals("authority\0actor\0" + ACTOR))
                                authorityReads.incrementAndGet();
                            return store.read(key, cancellation);
                        }

                        public CompletionStage<ZLinkStoreWriteResult> write(
                                ZLinkStoreWriteRequest request,
                                ZLinkStoreCancellation cancellation) {
                            return store.write(request, cancellation);
                        }

                        public CompletionStage<ZLinkStoreScanResult> scan(
                                ZLinkStoreScanRequest request,
                                ZLinkStoreCancellation cancellation) {
                            return store.scan(request, cancellation);
                        }
                    });
            options.configureDispatch().messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var mesh = options.addRouteMesh("game");
            mesh.listen("inproc://missing-local-" + System.nanoTime());
            mesh.objects()
                    .server()
                    .addEntrySpot(Entry.class)
                    .addActorFactory(
                            "probe",
                            ProbeActor.class,
                            Factory.class,
                            factory -> factory.disableRelocation());
            var streamOptions = options.addStreamNode("gateway");
            streamOptions.bind("tcp://127.0.0.1:0");
            streamOptions.enableActorDispatch();
            streamOptions.registerSession(Session.class);
            var captured = new CompletableFuture<ZLinkJavaRawSpotNode>();
            var capturedStream = new CompletableFuture<ZLinkBackendStreamSocket>();
            ZLinkMeshBackendAdapter meshBackend =
                    (context, name) -> {
                        var node =
                                new ZLinkJavaRawMeshNode(
                                        ((ZLinkJavaContext) context).nativeContext(),
                                        name,
                                        System::currentTimeMillis,
                                        nanoTime,
                                        ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL,
                                        ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT);
                        captured.complete((ZLinkJavaRawSpotNode) node.spotNode());
                        return node;
                    };
            var delegate = new ZLinkJavaBackendAdapterFactory();
            var backend =
                    (ZLinkBackendAdapterProvider)
                            Proxy.newProxyInstance(
                                    getClass().getClassLoader(),
                                    new Class<?>[] {ZLinkBackendAdapterProvider.class},
                                    (proxy, method, args) -> {
                                        try {
                                            var result = method.invoke(delegate, args);
                                            if (result instanceof ZLinkMeshBackendAdapter)
                                                return meshBackend;
                                            if (result
                                                    instanceof ZLinkStreamBackendAdapter adapter) {
                                                return (ZLinkStreamBackendAdapter)
                                                        (context, node) -> {
                                                            var socket =
                                                                    adapter.createStreamSocket(
                                                                            context, node);
                                                            capturedStream.complete(socket);
                                                            return socket;
                                                        };
                                            }
                                            return result;
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            runtime = ZLinkFrameworkRuntimeTestAccess.startHost(options, backend);
            try {
                spots = captured.get(3, TimeUnit.SECONDS);
                stream = capturedStream.get(3, TimeUnit.SECONDS);
                ref =
                        assertInstanceOf(
                                        ZLinkActorCreateResult.Created.class,
                                        runtime.actorManager()
                                                .create(ACTOR, "probe")
                                                .submit()
                                                .toCompletableFuture()
                                                .get(3, TimeUnit.SECONDS))
                                .actor();
                actor = created.poll(3, TimeUnit.SECONDS);
                assertNotNull(actor);
            } catch (Exception | Error failure) {
                runtime.close();
                throw failure;
            }
        }

        CompletableFuture<String> request(String value) {
            return runtime.actorClient()
                    .requestToActor(ACTOR, new Echo(value))
                    .submit(String.class)
                    .toCompletableFuture();
        }

        void sendHeartbeatPing(RoutingId session) throws Exception {
            try (var payload = Message.allocate(0)) {
                stream.sendAsync(session, ZLinkStreamHeader.HEARTBEAT_PING, List.of(payload))
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
            }
        }

        Socket connect() throws Exception {
            connected.clear();
            var endpoint =
                    URI.create(
                            runtime.listenerStatus(ZLinkListenerKind.STREAM, "gateway").endpoint());
            var socket = new Socket(endpoint.getHost(), endpoint.getPort());
            socket.setSoTimeout(3000);
            socket.getOutputStream()
                    .write(
                            ZLinkStreamFrameCodec.encode(
                                    new ZLinkStreamHeader("Open", Map.of(), Optional.empty()),
                                    "{}".getBytes(StandardCharsets.UTF_8)));
            return socket;
        }

        public void close() {
            runtime.close();
        }
    }

    public record Echo(String value) {}

    public static final class ProbeActor implements ZLinkActor {
        private final ZLinkActorContext context;
        final CompletableFuture<Void> entered = new CompletableFuture<>();
        volatile CompletableFuture<String> reply;

        ProbeActor(ZLinkActorContext context) {
            this.context = context;
        }

        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class Factory implements ZLinkActorFactory {
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            var actor = new ProbeActor(context);
            created.offer(actor);
            return CompletableFuture.completedFuture(actor);
        }
    }

    public static final class Entry implements ZLinkEntrySpot<ProbeActor> {
        private final ZLinkEntrySpotContext context;

        public Entry(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        public ZLinkEntrySpotContext context() {
            return context;
        }

        public void configure() {
            context.handlers().addHandler(EchoHandler.class);
        }

        public CompletionStage<Void> onJoinedActor(ProbeActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onLeaveActor(ProbeActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EchoHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(ProbeActor actor, Echo request) {
            actor.entered.complete(null);
            return actor.reply == null
                    ? CompletableFuture.completedFuture(request.value())
                    : actor.reply;
        }
    }

    public static final class Session implements ZLinkSession {
        private final ZLinkSessionContext context;

        public Session(ZLinkSessionContext context) {
            this.context = context;
        }

        public ZLinkSessionContext context() {
            return context;
        }

        public CompletionStage<Void> onConnected() {
            connected.offer(context.routingId().orElseThrow());
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
