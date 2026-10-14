package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.io.InputStream;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.BooleanSupplier;

final class StreamSessionTest {
    private static final int REPLACEMENT_ATTEMPTS = 1000;
    private static final int PARALLEL_RELAYS = 64;
    private static final int CLIENT_TIMEOUT_MILLIS = 3000;
    private static final String PARALLEL_SEND_PACKET = "StreamActorParallelSend";
    private static final String PARALLEL_REQUEST_PACKET = "StreamActorParallelRequest";
    private static final String HEARTBEAT_PING_PACKET = "$zlink.heartbeat.ping";
    private static final String HEARTBEAT_PONG_PACKET = "$zlink.heartbeat.pong";

    private enum CloseOrder {
        BEFORE_BIND,
        DURING_BIND,
        AFTER_BIND
    }

    @Test
    void streamNodeStartsWithoutExplicitSessionRelayAttach() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://play-router-" + System.nanoTime());
            node.objects()
                    .server()
                    .addSpotFactory(
                            "GameSpot", GameSpot.class, factory -> factory.disableRelocation());
        }
        {
            var stream = options.addStreamNode("gateway");
            stream.bind("inproc://gateway-" + System.nanoTime());
            stream.registerSession(GameSession.class);
        }
        ;

        try (ZLinkFrameworkRuntime ignored =
                RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {}
    }

    @Test
    void streamNodeDispatchesTcpRequestAndReplies() throws Exception {
        Zlink.version();
        EchoSession.reset();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        {
            var stream = options.addStreamNode("gateway");
            stream.bind("tcp://127.0.0.1:0");
            stream.registerSession(EchoSession.class);
        }
        ;

        try (ZLinkFrameworkRuntime runtime =
                        RuntimeTestSupport.startFramework(
                                options, new ZLinkJavaBackendAdapterFactory());
                Socket client = new Socket("127.0.0.1", streamPort(runtime))) {
            client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
            client.getOutputStream().write(frame(requestHeader(7L, "Ping"), bytes("ping")));
            client.getOutputStream().flush();

            byte[] prefix = client.getInputStream().readNBytes(6);
            assertEquals(6, prefix.length);
            ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
            int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
            int bodySize = prefixBuffer.getInt();
            byte[] header = readExact(client.getInputStream(), headerSize);
            byte[] body = readExact(client.getInputStream(), bodySize);

            assertTrue(header.length > 0);
            assertEquals(0xF2, Byte.toUnsignedInt(header[0]));
            assertEquals(3, Byte.toUnsignedInt(header[1]));
            assertEquals(7L, ByteBuffer.wrap(header, 4, Long.BYTES).getLong());
            assertEquals("\"pong\"", new String(body, StandardCharsets.UTF_8));
            assertTrue(EchoSession.dispatchedOnVirtualThread.get());
        }
    }

    @Test
    void streamNodeFailureRepliesErrorAndDoesNotBlockLaterRequests() throws Exception {
        Zlink.version();
        RecoveringSession.reset();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        {
            var stream = options.addStreamNode("gateway");
            stream.bind("tcp://127.0.0.1:0");
            stream.registerSession(RecoveringSession.class);
        }
        ;

        try (ZLinkFrameworkRuntime runtime =
                        RuntimeTestSupport.startFramework(
                                options, new ZLinkJavaBackendAdapterFactory());
                Socket client = new Socket("127.0.0.1", streamPort(runtime))) {
            client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);

            client.getOutputStream().write(frame(requestHeader(11L, "MustFail"), bytes("bad")));
            client.getOutputStream().flush();
            assertErrorReply(client.getInputStream(), 11L, "internal_failure", "public failure");

            client.getOutputStream().write(frame(requestHeader(12L, "Ping"), bytes("again")));
            client.getOutputStream().flush();
            assertReply(client, 12L, "\"pong\"");
        }
    }

    @Test
    void streamActorGatewayRelaysRequestAndReplies() throws Exception {
        try (ZLinkFrameworkRuntime runtime = startActorRelayRuntime();
                Socket client = new Socket("127.0.0.1", streamPort(runtime))) {
            client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);

            client.getOutputStream().write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
            client.getOutputStream().flush();
            assertBoundControl(client.getInputStream(), "player-1");
            assertReply(client, 1L, "\"bound\"");

            client.getOutputStream()
                    .write(frame(requestHeader(2L, "StreamActorEcho"), bytes("\"hello\"")));
            client.getOutputStream().flush();
            assertReply(client, 2L, "\"player-1:hello\"");
        }
    }

    @Test
    void sameActorRelayAfterPhysicalCloseAndReplacementBind() throws Exception {
        assertReplacementRelay(CloseOrder.BEFORE_BIND);
    }

    @Test
    void remotePhysicalCloseRetiresActorBoundSession() throws Exception {
        exerciseRemoteRetirement(false);
    }

    @Test
    void previousRemoteSessionTombstonePreservesReplacementBinding() throws Exception {
        exerciseRemoteRetirement(true);
    }

    private static void exerciseRemoteRetirement(boolean replaceBeforeCallbackTerminal)
            throws Exception {
        var entered = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        String redisEndpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        org.junit.jupiter.api.Assumptions.assumeTrue(
                redisEndpoint != null && !redisEndpoint.isBlank(),
                "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        GameEntrySpot.disconnectCallback =
                actor -> {
                    entered.complete(null);
                    return replaceBeforeCallbackTerminal
                            ? release
                            : CompletableFuture.completedFuture(null);
                };
        try (var locations =
                new systems.zlink.framework.locations.redis.ZLinkRedisLocationStore(
                        new systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions()
                                .setConnectionString(redisEndpoint)
                                .setKeyPrefix(
                                        "zlink:remote-retirement:"
                                                + java.util.UUID.randomUUID()))) {
            var relocations = new InMemoryRelocationStore();
            var actorOptions =
                    retirementActorOptions("remote-binding-actor", locations, relocations);
            try (var actorHost =
                    RuntimeTestSupport.startFramework(
                            actorOptions, new ZLinkJavaBackendAdapterFactory())) {
                actorHost
                        .actorManager()
                        .create("remote-player", "player")
                        .submit()
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                actorHost
                        .spotManager()
                        .getOrCreate("room-a", "retirement-room")
                        .submit()
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                actorHost
                        .actorClient()
                        .requestToActor("remote-player", new JoinUserSpotRequest("join"))
                        .submit(String.class)
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                assertEventually(
                        () ->
                                "room-a"
                                        .equals(
                                                actorHost
                                                        .actorClient()
                                                        .requestToActor(
                                                                "remote-player",
                                                                new MembershipProbe())
                                                        .submit(String.class)
                                                        .toCompletableFuture()
                                                        .join()));
                var sessionOptions = new DefaultZLinkFrameworkOptions();
                sessionOptions.addLocationStore(locations);
                var sessionNode = sessionOptions.addRouteMesh("game");
                sessionNode.objects().client();
                sessionNode
                        .listen("tcp://127.0.0.1:0")
                        .setRoutingId(RoutingId.from("remote-binding-session"));
                sessionNode
                        .peerConnections()
                        .connect(
                                actorHost
                                        .listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game")
                                        .endpoint());
                var stream = sessionOptions.addStreamNode("gateway");
                stream.bind("tcp://127.0.0.1:0");
                stream.enableActorDispatch();
                stream.registerSession(ActorRelaySession.class);
                try (var sessionHost =
                        RuntimeTestSupport.startFramework(
                                sessionOptions, new ZLinkJavaBackendAdapterFactory())) {
                    ActorRelaySession previousSession;
                    try (var client = new Socket("127.0.0.1", streamPort(sessionHost))) {
                        client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
                        client.getOutputStream()
                                .write(
                                        frame(
                                                requestHeader(1L, "Bind"),
                                                bytes("\"remote-player\"")));
                        client.getOutputStream().flush();
                        assertBoundControl(client.getInputStream(), "remote-player");
                        assertReply(client, 1L, "\"bound\"");
                        client.getOutputStream()
                                .write(
                                        frame(
                                                requestHeader(2L, "StreamActorEcho"),
                                                bytes("\"live\"")));
                        client.getOutputStream().flush();
                        assertReply(client, 2L, "\"remote-player:live\"");
                        assertEquals(
                                "BOUND",
                                actorHost
                                        .actorClient()
                                        .requestToActor("remote-player", new BoundSessionProbe())
                                        .submit(String.class)
                                        .toCompletableFuture()
                                        .get(3, TimeUnit.SECONDS));
                        assertRetirementPush(client);
                        previousSession = ActorRelaySession.lastConnected;
                    }
                    entered.get(3, TimeUnit.SECONDS);
                    if (replaceBeforeCallbackTerminal) {
                        try (var replacement = new Socket("127.0.0.1", streamPort(sessionHost))) {
                            replacement.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
                            replacement
                                    .getOutputStream()
                                    .write(
                                            frame(
                                                    requestHeader(3L, "Bind"),
                                                    bytes("\"remote-player\"")));
                            replacement.getOutputStream().flush();
                            assertBoundControl(replacement.getInputStream(), "remote-player");
                            assertReply(replacement, 3L, "\"bound\"");
                            release.complete(null);
                            assertEventually(
                                    () -> previousSession.context().actors().bound().isEmpty());
                            assertEquals(
                                    "BOUND",
                                    actorHost
                                            .actorClient()
                                            .requestToActor(
                                                    "remote-player", new BoundSessionProbe())
                                            .submit(String.class)
                                            .toCompletableFuture()
                                            .get(3, TimeUnit.SECONDS));
                            assertRetirementPush(replacement);
                            replacement
                                    .getOutputStream()
                                    .write(
                                            frame(
                                                    requestHeader(4L, "StreamActorEcho"),
                                                    bytes("\"replacement\"")));
                            replacement.getOutputStream().flush();
                            assertReply(replacement, 4L, "\"remote-player:replacement\"");
                        }
                        return;
                    }
                    assertEventually(
                            () ->
                                    "INVALID_OPERATION"
                                            .equals(
                                                    actorHost
                                                            .actorClient()
                                                            .requestToActor(
                                                                    "remote-player",
                                                                    new BoundSessionProbe())
                                                            .submit(String.class)
                                                            .toCompletableFuture()
                                                            .join()));
                    var targetOptions =
                            retirementActorOptions("remote-binding-target", locations, relocations);
                    try (var target =
                            RuntimeTestSupport.startFramework(
                                    targetOptions, new ZLinkJavaBackendAdapterFactory())) {
                        assertEventually(target::isReady);
                        assertEventually(
                                () ->
                                        actorHost
                                                .routeMeshRuntime()
                                                .snapshot("game")
                                                .peers()
                                                .stream()
                                                .anyMatch(
                                                        peer ->
                                                                peer.nodeRid()
                                                                                .equals(
                                                                                        RoutingId
                                                                                                .from(
                                                                                                        "remote-binding-target"))
                                                                        && peer.state()
                                                                                == systems.zlink
                                                                                        .framework
                                                                                        .monitoring
                                                                                        .ZLinkPeerState
                                                                                        .READY));
                        target.actorManager()
                                .create("target-ready-player", "player")
                                .submit()
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS);
                        assertEquals(
                                "",
                                actorHost
                                        .actorClient()
                                        .requestToActor(
                                                "target-ready-player", new MembershipProbe())
                                        .submit(String.class)
                                        .toCompletableFuture()
                                        .get(3, TimeUnit.SECONDS));
                        var relocation =
                                actorHost
                                        .relocate(
                                                new systems.zlink.framework.runtime.host
                                                        .ZLinkFrameworkRelocationOptions(
                                                        systems.zlink.framework.runtime.host
                                                                .ZLinkFrameworkRelocationMode
                                                                .PLANNED_MAINTENANCE,
                                                        null,
                                                        java.time.Duration.ofSeconds(3)))
                                        .toCompletableFuture()
                                        .get(5, TimeUnit.SECONDS);
                        assertEquals(
                                systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome
                                        .RELOCATED,
                                relocation.outcome(),
                                relocation::toString);
                        assertEventually(
                                () -> actorHost.status().safeToShutdown(),
                                actorOptions
                                        .configureLocations()
                                        .messageFollowDuration()
                                        .plusMillis(CLIENT_TIMEOUT_MILLIS));
                        assertEquals(
                                "INVALID_OPERATION",
                                target.actorClient()
                                        .requestToActor("remote-player", new BoundSessionProbe())
                                        .submit(String.class)
                                        .toCompletableFuture()
                                        .get(3, TimeUnit.SECONDS));
                    }
                } finally {
                    release.complete(null);
                    GameEntrySpot.disconnectCallback =
                            actor -> CompletableFuture.completedFuture(null);
                }
            }
        }
    }

    private static void assertRetirementPush(Socket client) throws Exception {
        var prefix = ByteBuffer.wrap(readExact(client.getInputStream(), 6));
        var header =
                ZLinkStreamHeaderCodec.decodeOrPlain(
                        readExact(client.getInputStream(), Short.toUnsignedInt(prefix.getShort())));
        var body = readExact(client.getInputStream(), prefix.getInt());
        assertEquals(ZLinkStreamMessageKind.SEND, header.kind());
        assertEquals("StreamActorPush", header.packetName());
        assertEquals("{\"value\":\"probe\"}", new String(body, StandardCharsets.UTF_8));
    }

    private static DefaultZLinkFrameworkOptions retirementActorOptions(
            String routingId,
            systems.zlink.framework.locationprovider.ZLinkLocationStore locations,
            InMemoryRelocationStore relocations) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(StreamSessionTest.class);
        options.addLocationStore(locations);
        options.addRelocationStore(relocations);
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        var node = options.addRouteMesh("game");
        node.listen("tcp://127.0.0.1:0").setRoutingId(RoutingId.from(routingId));
        node.objects().server().addEntrySpot(GameEntrySpot.class);
        node.objects()
                .server()
                .addActorFactory(
                        "player",
                        PlayerActor.class,
                        PlayerActorFactory.class,
                        factory -> factory.preserveStateWith(RetirementStateAdapter.class));
        node.objects()
                .server()
                .addSpotFactory(
                        "retirement-room",
                        UserSpot.class,
                        factory -> factory.preserveStateWith(RetirementSpotStateAdapter.class));
        return options;
    }

    public static final class RetirementSpotStateAdapter
            implements systems.zlink.framework.spots.ZLinkSpotRelocationAdapter<UserSpot> {
        @Override
        public CompletionStage<byte[]> capture(
                UserSpot spot,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                UserSpot spot,
                byte[] state,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            assertEquals(0, state.length);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RetirementStateAdapter
            implements systems.zlink.framework.actors.ZLinkActorRelocationAdapter<PlayerActor> {
        @Override
        public CompletionStage<byte[]> capture(
                PlayerActor actor,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            return CompletableFuture.completedFuture(new byte[0]);
        }

        @Override
        public CompletionStage<Void> restore(
                PlayerActor actor,
                byte[] state,
                systems.zlink.framework.actors.ZLinkRelocationCancellation cancellation) {
            assertEquals(0, state.length);
            return CompletableFuture.completedFuture(null);
        }
    }

    @Test
    void sameActorRelayAfterReplacementBindBeforePhysicalClose() throws Exception {
        assertReplacementRelay(CloseOrder.AFTER_BIND);
    }

    @Test
    void sameActorRelayWhilePhysicalCloseOverlapsReplacementBind() throws Exception {
        assertReplacementRelay(CloseOrder.DURING_BIND);
    }

    @Test
    void sameSessionConcurrentRelaysPreserveAcceptedSequence() throws Exception {
        try (ZLinkFrameworkRuntime runtime = startActorRelayRuntime();
                Socket client = new Socket("127.0.0.1", streamPort(runtime))) {
            client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
            client.getOutputStream().write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
            client.getOutputStream().flush();
            assertBoundControl(client.getInputStream(), "player-1");
            assertReply(client, 1L, "\"bound\"");
            for (int iteration = 0; iteration < REPLACEMENT_ATTEMPTS; iteration++) {
                CompletableFuture<Void> completed = new CompletableFuture<>();
                ActorRelaySession.parallelRelayCompleted = completed;
                byte[] header =
                        ZLinkStreamHeaderCodec.encode(
                                new ZLinkStreamHeader(
                                        ZLinkStreamMessageKind.SEND,
                                        ZLinkStreamCodec.JSON,
                                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                        Optional.empty(),
                                        PARALLEL_SEND_PACKET,
                                        Map.of(),
                                        Optional.empty()));
                client.getOutputStream().write(frame(header, bytes("\"hello\"")));
                client.getOutputStream().flush();
                completed.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
            }
        }
    }

    @Test
    void sameSessionConcurrentRequestsPreserveAcceptedSequenceAfterBind() throws Exception {
        try (ZLinkFrameworkRuntime runtime = startActorRelayRuntime();
                Socket client = new Socket("127.0.0.1", streamPort(runtime))) {
            client.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
            client.getOutputStream().write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
            client.getOutputStream().flush();
            assertBoundControl(client.getInputStream(), "player-1");
            assertReply(client, 1L, "\"bound\"");
            client.getOutputStream()
                    .write(frame(requestHeader(2L, "StreamActorEcho"), bytes("\"hello\"")));
            client.getOutputStream().flush();
            assertReply(client, 2L, "\"player-1:hello\"");
            for (int iteration = 0; iteration < REPLACEMENT_ATTEMPTS; iteration++) {
                CompletableFuture<Void> completed = new CompletableFuture<>();
                ActorRelaySession.parallelRelayCompleted = completed;
                long requestSequence = iteration + 3L;
                client.getOutputStream()
                        .write(
                                frame(
                                        requestHeader(requestSequence, PARALLEL_REQUEST_PACKET),
                                        bytes("\"hello\"")));
                client.getOutputStream().flush();
                for (int reply = 0; reply < PARALLEL_RELAYS; reply++) {
                    assertReply(client, requestSequence, "\"player-1:hello\"");
                }
                completed.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
            }
        }
    }

    private static void assertReplacementRelay(CloseOrder closeOrder) throws Exception {
        try (ZLinkFrameworkRuntime runtime = startActorRelayRuntime()) {
            Socket previous = null;
            try {
                for (int iteration = 0; iteration < REPLACEMENT_ATTEMPTS; iteration++) {
                    try (Socket next = new Socket("127.0.0.1", streamPort(runtime))) {
                        next.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
                        if (closeOrder == CloseOrder.BEFORE_BIND && previous != null) {
                            previous.close();
                            previous = null;
                        }
                        next.getOutputStream()
                                .write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
                        next.getOutputStream().flush();
                        CompletableFuture<Void> physicalClose = null;
                        if (closeOrder == CloseOrder.DURING_BIND && previous != null) {
                            Socket closing = previous;
                            physicalClose =
                                    CompletableFuture.runAsync(
                                            () -> {
                                                try {
                                                    closing.close();
                                                } catch (java.io.IOException failure) {
                                                    throw new java.io.UncheckedIOException(failure);
                                                }
                                            });
                            previous = null;
                        }
                        assertBoundControl(next.getInputStream(), "player-1");
                        assertReply(next, 1L, "\"bound\"");
                        if (previous != null) {
                            previous.close();
                            previous = null;
                        }
                        next.getOutputStream()
                                .write(
                                        frame(
                                                requestHeader(2L, "StreamActorEcho"),
                                                bytes("\"hello\"")));
                        next.getOutputStream().flush();
                        assertReply(next, 2L, "\"player-1:hello\"");
                        if (physicalClose != null) {
                            physicalClose.get(CLIENT_TIMEOUT_MILLIS, TimeUnit.MILLISECONDS);
                        }
                    }
                    // Keep A bound until the next iteration accepts and binds B.
                    previous = new Socket("127.0.0.1", streamPort(runtime));
                    previous.setSoTimeout(CLIENT_TIMEOUT_MILLIS);
                    previous.getOutputStream()
                            .write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
                    previous.getOutputStream().flush();
                    assertBoundControl(previous.getInputStream(), "player-1");
                    assertReply(previous, 1L, "\"bound\"");
                    previous.getOutputStream()
                            .write(frame(requestHeader(2L, "StreamActorEcho"), bytes("\"hello\"")));
                    previous.getOutputStream().flush();
                    assertReply(previous, 2L, "\"player-1:hello\"");
                }
            } finally {
                if (previous != null) {
                    previous.close();
                }
            }
        }
    }

    private static ZLinkFrameworkRuntime startActorRelayRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        options.addHandlersFromPackageOf(StreamSessionTest.class);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://stream-play-" + System.nanoTime())
                    .setRoutingId(RoutingId.from("play-node"));
            node.objects().server().addEntrySpot(GameEntrySpot.class);
            node.objects()
                    .server()
                    .addActorFactory(
                            "player",
                            PlayerActor.class,
                            PlayerActorFactory.class,
                            factory -> factory.disableRelocation());
        }
        {
            var stream = options.addStreamNode("gateway");
            stream.bind("tcp://127.0.0.1:0");
            stream.enableActorDispatch();
            stream.registerSession(ActorRelaySession.class);
        }
        ;

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class GameEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
        static java.util.function.Function<ZLinkActor, CompletionStage<Void>> disconnectCallback =
                actor -> CompletableFuture.completedFuture(null);

        @Override
        public CompletionStage<Void> onDisconnectActor(ZLinkActor actor) {
            return disconnectCallback.apply(actor);
        }

        private final ZLinkEntrySpotContext context;

        public GameEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(JoinUserSpotHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class UserSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public CompletionStage<Void> onDisconnectActor(ZLinkActor actor) {
            return GameEntrySpot.disconnectCallback.apply(actor);
        }

        static final AtomicBoolean lastLeave = new AtomicBoolean();
        private final ZLinkSpotContext context;

        public UserSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(LeaveUserSpotHandler.class);
        }

        @Override
        public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(ZLinkSpotActorJoinResult.accept("joined"));
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            lastLeave.set(actor.context().spotId().isEmpty());
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class PlayerActor implements ZLinkActor {
        private final ZLinkActorContext context;

        PlayerActor(ZLinkActorContext context) {
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }
    }

    public static final class PlayerActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new PlayerActor(context));
        }
    }

    public static final class ActorEchoHandler {
        @ZLinkSpotActorRequest(packetName = "MembershipProbe")
        public CompletionStage<String> membership(PlayerActor actor, MembershipProbe request) {
            return CompletableFuture.completedFuture(actor.context().spotId().orElse(""));
        }

        @ZLinkSpotActorRequest(packetName = "BoundSessionProbe")
        public CompletionStage<String> probe(PlayerActor actor, BoundSessionProbe request) {
            try {
                return actor.context()
                        .boundSession()
                        .send(new StreamActorPush("probe"))
                        .submit()
                        .handle(
                                (ignored, failure) ->
                                        failure == null ? "BOUND" : bindingFailureKind(failure));
            } catch (systems.zlink.framework.errors.ZLinkFrameworkException failure) {
                return CompletableFuture.completedFuture(failure.kind().name());
            }
        }

        private static String bindingFailureKind(Throwable failure) {
            while (failure instanceof java.util.concurrent.CompletionException
                    && failure.getCause() != null) {
                failure = failure.getCause();
            }
            return failure
                            instanceof
                            systems.zlink.framework.errors.ZLinkFrameworkException frameworkFailure
                    ? frameworkFailure.kind().name()
                    : failure.getClass().getSimpleName();
        }

        @ZLinkSpotActorSend(packetName = PARALLEL_SEND_PACKET)
        public CompletionStage<Void> handleSend(PlayerActor actor, String request) {
            return CompletableFuture.completedFuture(null);
        }

        @ZLinkSpotActorRequest(packetName = PARALLEL_REQUEST_PACKET)
        public CompletionStage<String> handleParallelRequest(PlayerActor actor, String request) {
            return CompletableFuture.completedFuture(actor.context().actorId() + ":" + request);
        }

        @ZLinkSpotActorRequest(packetName = "StreamActorEcho")
        public CompletionStage<String> handle(PlayerActor actor, String request) {
            return CompletableFuture.completedFuture(actor.context().actorId() + ":" + request);
        }

        @ZLinkSpotActorRequest(packetName = "StreamActorEchoWithPush")
        public CompletionStage<String> handleWithPush(PlayerActor actor, String request) {
            actor.context().boundSession().send(new StreamActorPush("push:" + request)).submit();
            return CompletableFuture.completedFuture(actor.context().actorId() + ":" + request);
        }
    }

    @ZLinkPacket("StreamActorPush")
    public record StreamActorPush(String value) {}

    @ZLinkPacket("BoundSessionProbe")
    public record BoundSessionProbe() {}

    @ZLinkPacket("MembershipProbe")
    public record MembershipProbe() {}

    @ZLinkPacket("JoinUserSpot")
    public record JoinUserSpotRequest(String value) {}

    public static final class JoinUserSpotHandler
            implements ZLinkEntrySpotActorRequestHandler<
                    GameEntrySpot, PlayerActor, JoinUserSpotRequest, String> {
        @Override
        public CompletionStage<String> handle(
                GameEntrySpot spot,
                PlayerActor actor,
                ZLinkMessageContext context,
                JoinUserSpotRequest request) {
            actor.context().joinSpot("room-a", request.value()).defer();
            return CompletableFuture.completedFuture("deferred");
        }
    }

    public static final class LeaveUserSpotHandler {
        @ZLinkSpotActorRequest(packetName = "LeaveUserSpot")
        public CompletionStage<String> handle(
                UserSpot spot, PlayerActor actor, ZLinkMessageContext context, String request) {
            return spot.context().leaveActor(actor).thenApply(ignored -> "left");
        }
    }

    public static final class GameSession implements ZLinkSession {
        @Override
        public ZLinkSessionContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class EchoSession implements ZLinkSession {
        static final AtomicBoolean dispatchedOnVirtualThread = new AtomicBoolean();
        private final ZLinkSessionContext context;

        public EchoSession(ZLinkSessionContext context) {
            this.context = context;
        }

        static void reset() {
            dispatchedOnVirtualThread.set(false);
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            dispatchedOnVirtualThread.set(Thread.currentThread().isVirtual());
            if (!"Ping".equals(dispatch.packetName())) {
                throw new IllegalArgumentException("unexpected packet: " + dispatch.packetName());
            }
            context.client().reply("pong").submit();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class RecoveringSession implements ZLinkSession {
        static final AtomicBoolean recovered = new AtomicBoolean();
        private final ZLinkSessionContext context;

        public RecoveringSession(ZLinkSessionContext context) {
            this.context = context;
        }

        static void reset() {
            recovered.set(false);
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if ("MustFail".equals(dispatch.packetName())) {
                throw new IllegalStateException("public failure");
            }
            recovered.set(true);
            context.client().reply("pong").submit();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ActorRelaySession implements ZLinkSession {
        static volatile ActorRelaySession lastConnected;
        static volatile CompletableFuture<Void> parallelRelayCompleted;
        private final ZLinkSessionContext context;
        private final ZLinkActorManager actors;

        public ActorRelaySession(ZLinkSessionContext context, ZLinkActorManager actors) {
            lastConnected = this;
            this.context = context;
            this.actors = actors;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage payload) {
            if (PARALLEL_SEND_PACKET.equals(dispatch.packetName())
                    || PARALLEL_REQUEST_PACKET.equals(dispatch.packetName())) {
                var actor = context.actors().bound().get(0);
                CompletableFuture<?>[] relays = new CompletableFuture<?>[PARALLEL_RELAYS];
                for (int index = 0; index < relays.length; index++) {
                    relays[index] =
                            CompletableFuture.supplyAsync(() -> actor.relay(dispatch, payload))
                                    .thenCompose(stage -> stage)
                                    .toCompletableFuture();
                }
                CompletableFuture<Void> observed = parallelRelayCompleted;
                return CompletableFuture.allOf(relays)
                        .whenComplete(
                                (ignored, failure) -> {
                                    if (failure == null) {
                                        observed.complete(null);
                                    } else {
                                        observed.completeExceptionally(failure);
                                    }
                                });
            }
            if ("Bind".equals(dispatch.packetName())) {
                String actorId = payload.decode(String.class);
                return actors.getOrCreate(actorId, "player")
                        .submit()
                        .thenCompose(
                                result -> {
                                    var actor =
                                            switch (result) {
                                                case ZLinkActorCreateResult.Created created ->
                                                        created.actor();
                                                case ZLinkActorCreateResult.Existing existing ->
                                                        existing.actor();
                                                case ZLinkActorCreateResult.Rejected rejected ->
                                                        throw new IllegalStateException(
                                                                "actor creation rejected: "
                                                                        + rejected.reply());
                                            };
                                    return context.actors().bind(actor);
                                })
                        .thenRun(() -> context.client().reply("bound").submit());
            }
            return context.actors().bound().get(0).relay(payload);
        }
    }

    private static int streamPort(ZLinkFrameworkRuntime runtime) {
        return URI.create(runtime.listenerStatus(ZLinkListenerKind.STREAM, "gateway").endpoint())
                .getPort();
    }

    private static byte[] requestHeader(long requestSeq, String packetName) {
        return ZLinkStreamHeaderCodec.encode(
                new ZLinkStreamHeader(
                        ZLinkStreamMessageKind.REQUEST,
                        ZLinkStreamCodec.RAW,
                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                        Optional.of(requestSeq),
                        packetName,
                        Map.of(),
                        Optional.empty()));
    }

    private static byte[] frame(byte[] header, byte[] body) {
        ByteBuffer buffer = ByteBuffer.allocate(6 + header.length + body.length);
        buffer.putShort((short) header.length);
        buffer.putInt(body.length);
        buffer.put(header);
        buffer.put(body);
        return buffer.array();
    }

    private static byte[] readExact(InputStream input, int size) throws Exception {
        byte[] bytes = input.readNBytes(size);
        assertEquals(size, bytes.length);
        return bytes;
    }

    private static void assertReply(Socket client, long requestSeq, String expectedBody)
            throws Exception {
        InputStream input = client.getInputStream();
        java.io.OutputStream output = client.getOutputStream();
        while (true) {
            byte[] prefix = input.readNBytes(6);
            assertEquals(6, prefix.length);
            ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
            int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
            int bodySize = prefixBuffer.getInt();
            byte[] header = readExact(input, headerSize);
            byte[] body = readExact(input, bodySize);

            ZLinkStreamHeader decoded = ZLinkStreamHeaderCodec.decodeOrPlain(header);
            if (decoded.kind() == ZLinkStreamMessageKind.CONTROL
                    && HEARTBEAT_PING_PACKET.equals(decoded.packetName())) {
                assertEquals(0, body.length);
                ZLinkStreamHeader pong =
                        new ZLinkStreamHeader(
                                ZLinkStreamMessageKind.CONTROL,
                                ZLinkStreamCodec.RAW,
                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                Optional.empty(),
                                HEARTBEAT_PONG_PACKET,
                                Map.of());
                output.write(frame(ZLinkStreamHeaderCodec.encode(pong), new byte[0]));
                output.flush();
                continue;
            }
            assertEquals(
                    3,
                    Byte.toUnsignedInt(header[1]),
                    () ->
                            "unexpected frame kind header="
                                    + ZLinkStreamHeaderCodec.decodeOrPlain(header)
                                    + " body="
                                    + new String(body, StandardCharsets.UTF_8));
            assertEquals(0xF2, Byte.toUnsignedInt(header[0]));
            assertEquals(requestSeq, ByteBuffer.wrap(header, 4, Long.BYTES).getLong());
            assertEquals(expectedBody, new String(body, StandardCharsets.UTF_8));
            return;
        }
    }

    private static void assertBoundControl(InputStream input, String actorId) throws Exception {
        byte[] prefix = readExact(input, 6);
        ByteBuffer sizes = ByteBuffer.wrap(prefix);
        byte[] headerBytes = readExact(input, Short.toUnsignedInt(sizes.getShort()));
        byte[] body = readExact(input, sizes.getInt());
        ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(headerBytes);
        assertEquals(ZLinkStreamMessageKind.CONTROL, header.kind());
        assertEquals("$zlink.actor.bound", header.packetName());
        ByteBuffer payload = ByteBuffer.wrap(body);
        assertEquals(1, Byte.toUnsignedInt(payload.get()));
        assertTrue(Short.toUnsignedInt(payload.getShort()) > 0);
        int idLength = Byte.toUnsignedInt(payload.get());
        assertEquals(idLength, payload.remaining());
        assertEquals(actorId, new String(body, 4, idLength, StandardCharsets.UTF_8));
    }

    private static void assertErrorReply(
            InputStream input, long requestSeq, String expectedCode, String expectedMessage)
            throws Exception {
        byte[] prefix = input.readNBytes(6);
        assertEquals(6, prefix.length);
        ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
        int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
        int bodySize = prefixBuffer.getInt();
        byte[] header = readExact(input, headerSize);
        byte[] body = readExact(input, bodySize);

        if (Byte.toUnsignedInt(header[1]) == 5) {
            assertErrorReply(input, requestSeq, expectedCode, expectedMessage);
            return;
        }
        assertEquals(0xF2, Byte.toUnsignedInt(header[0]));
        assertEquals(4, Byte.toUnsignedInt(header[1]));
        assertEquals(1, Byte.toUnsignedInt(header[2]));
        assertEquals(requestSeq, ByteBuffer.wrap(header, 4, Long.BYTES).getLong());
        assertEquals(0, Byte.toUnsignedInt(header[12]));
        var error = new ObjectMapper().readTree(body);
        assertEquals(expectedCode, error.get("code").asText());
        assertEquals(expectedMessage, error.get("message").asText());
    }

    private static void assertSend(
            InputStream input, String expectedPacketName, String expectedBody) throws Exception {
        byte[] prefix = input.readNBytes(6);
        assertEquals(6, prefix.length);
        ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
        int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
        int bodySize = prefixBuffer.getInt();
        byte[] header = readExact(input, headerSize);
        byte[] body = readExact(input, bodySize);

        assertEquals(0xF2, Byte.toUnsignedInt(header[0]));
        assertEquals(1, Byte.toUnsignedInt(header[1]));
        int nameLengthOffset = 4;
        int nameLength = Byte.toUnsignedInt(header[nameLengthOffset]);
        String packetName =
                new String(header, nameLengthOffset + 1, nameLength, StandardCharsets.UTF_8);
        assertEquals(expectedPacketName, packetName);
        assertEquals(expectedBody, new String(body, StandardCharsets.UTF_8));
    }

    private static void assertEventually(BooleanSupplier condition) throws Exception {
        assertEventually(condition, java.time.Duration.ofMillis(CLIENT_TIMEOUT_MILLIS));
    }

    private static void assertEventually(
            BooleanSupplier condition, java.time.Duration observationWindow) throws Exception {
        long deadline = System.nanoTime() + observationWindow.toNanos();
        while (System.nanoTime() < deadline) {
            if (condition.getAsBoolean()) {
                return;
            }
            Thread.sleep(10);
        }
        assertTrue(condition.getAsBoolean());
    }

    private static byte[] bytes(String value) {
        return value.getBytes(StandardCharsets.UTF_8);
    }
}
