package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorCreateResult;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.io.InputStream;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

final class SessionActorsRuntimeIntegrationTest {
    private static final long REMOTE_BIND_WAIT_SECONDS = 10;
    static final LinkedBlockingQueue<String> actorRelayRequests = new LinkedBlockingQueue<>();
    static final AtomicReference<ZLinkActorManager> channelActors = new AtomicReference<>();
    private static final LinkedBlockingQueue<RoutingId> connectedSessions =
            new LinkedBlockingQueue<>();

    private static java.util.logging.FileHandler closeFlow;

    @org.junit.jupiter.api.BeforeAll
    static void captureCloseFlow() throws Exception {
        closeFlow = new java.util.logging.FileHandler("build/bound-session-close.flow");
        closeFlow.setFormatter(new java.util.logging.SimpleFormatter());
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .addHandler(closeFlow);
    }

    @org.junit.jupiter.api.AfterAll
    static void closeCloseFlow() {
        java.util.logging.Logger.getLogger(
                        "systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer")
                .removeHandler(closeFlow);
        closeFlow.close();
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false",
        "false,true",
        "true,false",
        "true,true"
    })
    void actorHandlerClosesPhysicalBoundSession(boolean remote, boolean join) throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var ownerOptions = new DefaultZLinkFrameworkOptions();
        ownerOptions
                .configureDispatch()
                .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode.NORMAL);
        ownerOptions.addLocationStore(locations);
        var ownerNode = ownerOptions.addRouteMesh("game");
        ownerNode.listen("tcp://127.0.0.1:0");
        ownerNode
                .objects()
                .server()
                .addEntrySpot(GameEntrySpot.class)
                .addActorFactory(
                        "player",
                        PlayerActor.class,
                        PlayerActorFactory.class,
                        factory -> factory.disableRelocation())
                .addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation());
        if (!remote) {
            var stream = ownerOptions.addStreamNode("gateway");
            stream.bind("tcp://127.0.0.1:0");
            stream.enableActorDispatch();
            stream.registerSession(GameSession.class);
        }
        try (var owner =
                RuntimeTestSupport.startFramework(
                        ownerOptions, new ZLinkJavaBackendAdapterFactory())) {
            ZLinkFrameworkRuntime sessionOwner = owner;
            if (remote) {
                var sessionOptions = new DefaultZLinkFrameworkOptions();
                sessionOptions
                        .configureDispatch()
                        .messageFlow(
                                systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
                                        .NORMAL);
                sessionOptions.addLocationStore(locations);
                var node = sessionOptions.addRouteMesh("game");
                node.listen("tcp://127.0.0.1:0");
                node.objects().client();
                node.peerConnections()
                        .connect(
                                owner.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game")
                                        .endpoint());
                var stream = sessionOptions.addStreamNode("gateway");
                stream.bind("tcp://127.0.0.1:0");
                stream.enableActorDispatch();
                stream.registerSession(GameSession.class);
                sessionOwner =
                        RuntimeTestSupport.startFramework(
                                sessionOptions, new ZLinkJavaBackendAdapterFactory());
            }
            try (Socket client = connectStream(sessionOwner, "gateway")) {
                client.setSoTimeout(3000);
                var created =
                        assertInstanceOf(
                                ZLinkActorCreateResult.Created.class,
                                owner.actorManager()
                                        .getOrCreate("close-player", "player")
                                        .submit()
                                        .toCompletableFuture()
                                        .get(3, TimeUnit.SECONDS));
                var watched = (PlayerActor) managedActor(owner, "close-player", "player");
                var sessionActors = sessionOwner.sessionActors("gateway", connectedSession());
                sessionActors
                        .bindOrGet(created.actor())
                        .toCompletableFuture()
                        .get(3, TimeUnit.SECONDS);
                var input = client.getInputStream();
                var prefix = ByteBuffer.wrap(input.readNBytes(6));
                input.readNBytes(Short.toUnsignedInt(prefix.getShort()) + prefix.getInt());
                if (join) {
                    owner.spotManager()
                            .getOrCreate("close-room", "GameSpot")
                            .submit()
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
                    owner.actorClient()
                            .requestToActor("close-player", new CloseJoinRequest())
                            .submit(String.class)
                            .toCompletableFuture()
                            .get(3, TimeUnit.SECONDS);
                    assertEquals(
                            "close-room",
                            owner.actorClient()
                                    .requestToActor("close-player", new CloseWhereRequest())
                                    .submit(String.class)
                                    .toCompletableFuture()
                                    .get(3, TimeUnit.SECONDS));
                }
                assertEquals(
                        "closed",
                        owner.actorClient()
                                .requestToActor("close-player", new CloseRequest())
                                .submit(String.class)
                                .toCompletableFuture()
                                .get(3, TimeUnit.SECONDS));
                assertEquals(
                        -1,
                        input.read(),
                        "Actor disconnect must close the physical client connection");
                watched.disconnected.get(3, TimeUnit.SECONDS);
                assertEquals(1, watched.disconnectCount.get());
                long cleanupDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                while (sessionActors.find("close-player").isPresent()
                        && System.nanoTime() < cleanupDeadline) Thread.sleep(10);
                org.junit.jupiter.api.Assertions.assertTrue(
                        sessionActors.find("close-player").isEmpty());
                var unbound =
                        org.junit.jupiter.api.Assertions.assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () ->
                                        owner.actorClient()
                                                .requestToActor("close-player", new CloseRequest())
                                                .submit(String.class)
                                                .toCompletableFuture()
                                                .get(3, TimeUnit.SECONDS));
                assertEquals(
                        systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                        ((systems.zlink.framework.errors.ZLinkFrameworkException)
                                        unbound.getCause())
                                .kind());
            } finally {
                if (remote) sessionOwner.close();
            }
        }
    }

    @Test
    void remoteBindingAcknowledgesSessionContext() throws Exception {
        var locations = new ZLinkInMemoryLocationStore();
        var targetOptions = new DefaultZLinkFrameworkOptions();
        targetOptions.addLocationStore(locations);
        targetOptions
                .addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from("implicit-entry-owner"))
                .objects()
                .server()
                .addEntrySpot(ActorCreationEntry.class)
                .addActorFactory(
                        "player",
                        PlayerActor.class,
                        PlayerActorFactory.class,
                        factory -> factory.disableRelocation());
        try (var target =
                RuntimeTestSupport.startFramework(
                        targetOptions, new ZLinkJavaBackendAdapterFactory())) {
            var sourceOptions = new DefaultZLinkFrameworkOptions();
            sourceOptions.addLocationStore(locations);
            var sourceNode = sourceOptions.addRouteMesh("game");
            sourceNode
                    .listen("tcp://127.0.0.1:0")
                    .setRoutingId(RoutingId.from("implicit-entry-client"));
            sourceNode.objects().client();
            sourceNode
                    .peerConnections()
                    .connect(
                            target.listenerStatus(ZLinkListenerKind.ROUTE_MESH, "game").endpoint());
            var stream = sourceOptions.addStreamNode("gateway");
            stream.bind("tcp://127.0.0.1:0");
            stream.enableActorDispatch();
            stream.registerSession(GameSession.class);
            try (var source =
                            RuntimeTestSupport.startFramework(
                                    sourceOptions, new ZLinkJavaBackendAdapterFactory());
                    Socket client = connectStream(source, "gateway")) {
                var created =
                        assertInstanceOf(
                                ZLinkActorCreateResult.Created.class,
                                source.actorManager()
                                        .getOrCreate("implicit-player", "player")
                                        .submit()
                                        .toCompletableFuture()
                                        .get(REMOTE_BIND_WAIT_SECONDS, TimeUnit.SECONDS));
                var bound =
                        source.sessionActors("gateway", connectedSession())
                                .bindOrGet(created.actor())
                                .toCompletableFuture()
                                .get(REMOTE_BIND_WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(created.actor().actorId(), bound.actorId());
            }
        }
    }

    @Test
    void bindUsesStreamActorGatewayBindingPath() throws Exception {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime();
                Socket client = connectStream(runtime, "gateway")) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            ZLinkSessionActorsRuntime sessionActors =
                    runtime.sessionActors("gateway", connectedSession());
            ZLinkSessionActor bound = sessionActors.bind(actor).toCompletableFuture().join();

            assertEquals("player-1", bound.actorId());
            assertEquals(Optional.of(bound), sessionActors.find("player-1"));
        }
    }

    @Test
    void bindCanRelayLocalManagedActorWithoutActorGatewayAttach() throws Exception {
        actorRelayRequests.clear();
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startLocalManagedStreamRuntime();
                Socket client = connectStream(runtime, "local")) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            var sessionActors = runtime.sessionActors("local", connectedSession());
            ZLinkSessionActor bound = sessionActors.bind(actor).toCompletableFuture().join();

            relayWithHeader(
                    sessionActors,
                    bound,
                    "ActorNotify",
                    ZLinkMessage.of(new ActorNotifyMessage("hello")));

            assertEquals("player-1:hello", awaitActorRelay("player-1:hello", 2, TimeUnit.SECONDS));
        }
    }

    @Test
    void channelRequestHandler_canCreateActorAndJoinEntrySpot() {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startChannelJoinRuntime()) {
            channelActors.set(runtime.actorManager());
            String actorId =
                    runtime.client()
                            .requestToChannel("play", new EnsureActorRequest("player-1"))
                            .timeout(Duration.ofSeconds(2))
                            .submit(String.class)
                            .toCompletableFuture()
                            .join();

            assertEquals("player-1", actorId);
        }
    }

    @Test
    void sessionAndPlayServers_relaySucceeds() throws Exception {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime();
                Socket client = connectStream(runtime, "gateway")) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            ZLinkSessionActorsRuntime sessionActors =
                    runtime.sessionActors("gateway", connectedSession());
            ZLinkSessionActor bound = sessionActors.bind(actor).toCompletableFuture().join();

            assertEquals("player-1", bound.actorId());
            assertEquals(Optional.of(bound), sessionActors.find("player-1"));
        }
    }

    @Test
    void playActorPush_followsBoundControlOnLiveClientStream() throws Exception {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime();
                Socket client = connectStream(runtime, "gateway")) {
            client.setSoTimeout(3000);
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            runtime.sessionActors("gateway", connectedSession())
                    .bind(actor)
                    .toCompletableFuture()
                    .join();

            InputStream input = client.getInputStream();
            byte[] boundPrefix = input.readNBytes(6);
            assertEquals(6, boundPrefix.length);
            ByteBuffer boundSizes = ByteBuffer.wrap(boundPrefix);
            byte[] boundHeader = input.readNBytes(Short.toUnsignedInt(boundSizes.getShort()));
            byte[] boundBody = input.readNBytes(boundSizes.getInt());
            assertEquals(
                    ZLinkStreamMessageKind.CONTROL,
                    ZLinkStreamHeaderCodec.decodeOrPlain(boundHeader).kind());
            assertEquals(
                    "$zlink.actor.bound",
                    ZLinkStreamHeaderCodec.decodeOrPlain(boundHeader).packetName());
            assertEquals(
                    "player-1",
                    new String(boundBody, 4, boundBody.length - 4, StandardCharsets.UTF_8));

            actor.context().boundSession().send("push").submit().toCompletableFuture().join();
            byte[] pushPrefix = input.readNBytes(6);
            assertEquals(6, pushPrefix.length);
            ByteBuffer pushSizes = ByteBuffer.wrap(pushPrefix);
            byte[] pushHeader = input.readNBytes(Short.toUnsignedInt(pushSizes.getShort()));
            byte[] pushBody = input.readNBytes(pushSizes.getInt());
            assertEquals(
                    ZLinkStreamMessageKind.SEND,
                    ZLinkStreamHeaderCodec.decodeOrPlain(pushHeader).kind());
            assertEquals("\"push\"", new String(pushBody, StandardCharsets.UTF_8));
        }
    }

    private static ZLinkActor managedActor(
            ZLinkFrameworkRuntime runtime, String actorId, String actorType) {
        return ((ZLinkActorRuntime) runtime.actorManager())
                .getOrCreateManagedActor(actorId, actorType)
                .toCompletableFuture()
                .join();
    }

    static Socket connectStream(ZLinkFrameworkRuntime runtime, String streamNode) throws Exception {
        connectedSessions.clear();
        int port =
                URI.create(runtime.listenerStatus(ZLinkListenerKind.STREAM, streamNode).endpoint())
                        .getPort();
        Socket client = new Socket("127.0.0.1", port);
        byte[] header =
                ZLinkStreamHeaderCodec.encode(
                        new ZLinkStreamHeader(
                                ZLinkStreamMessageKind.SEND,
                                ZLinkStreamCodec.RAW,
                                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                Optional.empty(),
                                "Open",
                                Map.of(),
                                Optional.empty()));
        byte[] body = "\"open\"".getBytes(StandardCharsets.UTF_8);
        ByteBuffer frame = ByteBuffer.allocate(6 + header.length + body.length);
        frame.putShort((short) header.length).putInt(body.length).put(header).put(body);
        client.getOutputStream().write(frame.array());
        client.getOutputStream().flush();
        return client;
    }

    static RoutingId connectedSession() throws Exception {
        RoutingId session = connectedSessions.poll(2, TimeUnit.SECONDS);
        if (session == null) {
            throw new TimeoutException("STREAM client did not connect");
        }
        return session;
    }

    private static ZLinkFrameworkRuntime startGatewayRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://play-" + System.nanoTime())
                    .setRoutingId(RoutingId.from("play-node"));
            node.objects()
                    .server()
                    .addSpotFactory(
                            "GameSpot", GameSpot.class, factory -> factory.disableRelocation());
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
            stream.registerSession(GameSession.class);
        }
        ;

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startLocalManagedStreamRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://local-play-" + System.nanoTime())
                    .setRoutingId(RoutingId.from("play-node"));
            node.objects().server().addEntrySpot(GameEntrySpot.class);
            node.objects()
                    .server()
                    .addSpotFactory(
                            "GameSpot", GameSpot.class, factory -> factory.disableRelocation());
            node.objects()
                    .server()
                    .addActorFactory(
                            "player",
                            PlayerActor.class,
                            PlayerActorFactory.class,
                            factory -> factory.disableRelocation());
        }
        {
            var stream = options.addStreamNode("local");
            stream.bind("tcp://127.0.0.1:0");
            stream.enableActorDispatch();
            stream.registerSession(GameSession.class);
        }
        ;

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startBoundGatewayRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        String suffix = Long.toString(System.nanoTime());
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://play-router-" + suffix).setRoutingId(RoutingId.from("play-node"));
            node.objects()
                    .server()
                    .addSpotFactory(
                            "GameSpot", GameSpot.class, factory -> factory.disableRelocation());
            node.objects().server().addEntrySpot(GameEntrySpot.class);
            node.objects()
                    .server()
                    .addActorFactory(
                            "player",
                            PlayerActor.class,
                            PlayerActorFactory.class,
                            factory -> factory.disableRelocation());
        }

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startChannelJoinRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        {
            var channel = options.addClientServerChannel("play").server().listen();
            options.addClientServerChannel("play").client();
            channel.addRequestHandler(
                    EnsureActorHandler.class, EnsureActorRequest.class, String.class);
        }
        ;
        {
            var node = options.addRouteMesh("game");
            node.listen("inproc://channel-play-" + System.nanoTime())
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

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    public static final class PlayerActor implements ZLinkActor {
        final CompletableFuture<Void> disconnected = new CompletableFuture<>();
        final java.util.concurrent.atomic.AtomicInteger disconnectCount =
                new java.util.concurrent.atomic.AtomicInteger();

        CompletionStage<Void> observeDisconnect() {
            disconnectCount.incrementAndGet();
            disconnected.complete(null);
            return CompletableFuture.completedFuture(null);
        }

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

    public static final class GameSpot implements ZLinkSpot<PlayerActor> {
        private final ZLinkSpotContext context;

        public GameSpot(ZLinkSpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSpotContext context() {
            return context;
        }

        @Override
        public void configure() {
            context.handlers().addHandler(ActorEchoHandler.class);
            context.handlers().addHandler(CloseHandler.class);
            context.handlers().addHandler(CloseWhereHandler.class);
            context.handlers().addHandler(ActorNotifyHandler.class);
        }

        @Override
        public CompletionStage<systems.zlink.framework.spots.ZLinkSpotActorJoinResult> onActorJoin(
                String actorId, ZLinkMessage request) {
            return CompletableFuture.completedFuture(
                    systems.zlink.framework.spots.ZLinkSpotActorJoinResult.accept());
        }

        @Override
        public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnectActor(PlayerActor actor) {
            return actor.observeDisconnect();
        }

        @Override
        public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class GameEntrySpot implements ZLinkEntrySpot<PlayerActor> {
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
            context.handlers().addHandler(ActorEchoHandler.class);
            context.handlers().addHandler(CloseHandler.class);
            context.handlers().addHandler(CloseJoinHandler.class);
            context.handlers().addHandler(CloseWhereHandler.class);
            context.handlers().addHandler(ActorNotifyHandler.class);
        }

        @Override
        public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnectActor(PlayerActor actor) {
            return actor.observeDisconnect();
        }

        @Override
        public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public record CloseRequest() {}

    public record CloseJoinRequest() {}

    public record CloseWhereRequest() {}

    public static final class CloseHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(PlayerActor actor, CloseRequest request) {
            return actor.context().boundSession().disconnect().thenApply(ignored -> "closed");
        }
    }

    public static final class CloseJoinHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(PlayerActor actor, CloseJoinRequest request) {
            actor.context().joinSpot("close-room").defer();
            return CompletableFuture.completedFuture("joined");
        }
    }

    public static final class CloseWhereHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(PlayerActor actor, CloseWhereRequest request) {
            return CompletableFuture.completedFuture(actor.context().spotId().orElse("entry"));
        }
    }

    public static final class ActorEchoHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<String> handle(PlayerActor actor, ActorEchoRequest request) {
            actorRelayRequests.offer(actor.context().actorId() + ":" + request.value());
            return CompletableFuture.completedFuture(request.value());
        }
    }

    public static final class ActorNotifyHandler {
        @ZLinkSpotActorSend
        public CompletionStage<Void> handle(PlayerActor actor, ActorNotifyMessage request) {
            actorRelayRequests.offer(actor.context().actorId() + ":" + request.value());
            return CompletableFuture.completedFuture(null);
        }
    }

    public record JsonRelayReq(String value) {}

    public record JsonRelayRes(String value) {}

    @ZLinkPacket("ActorEcho")
    public record ActorEchoRequest(String value) {}

    @ZLinkPacket("ActorNotify")
    public record ActorNotifyMessage(String value) {}

    @ZLinkPacket("Ensure")
    public record EnsureActorRequest(String actorId) {}

    public static final class DefaultJsonActorHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<JsonRelayRes> handle(PlayerActor actor, JsonRelayReq request) {
            actorRelayRequests.offer(actor.context().actorId() + ":" + request.value());
            return CompletableFuture.completedFuture(new JsonRelayRes(request.value()));
        }
    }

    private static String awaitActorRelay(String expected, long timeout, TimeUnit unit)
            throws InterruptedException, TimeoutException {
        long deadline = System.nanoTime() + unit.toNanos(timeout);
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                throw new TimeoutException();
            }
            String received = actorRelayRequests.poll(remaining, TimeUnit.NANOSECONDS);
            if (expected.equals(received)) {
                return received;
            }
        }
    }

    static String uniqueActorId(String prefix) {
        return prefix + "-" + Long.toUnsignedString(System.nanoTime(), 36);
    }

    static void relayWithHeader(
            ZLinkSessionActorsRuntime sessionActors,
            ZLinkSessionActor actor,
            String packetName,
            ZLinkMessage payload) {
        sessionActors.enterRelayDispatch(
                new ZLinkStreamHeader(packetName, Map.of(), Optional.empty()));
        try {
            actor.relay(payload).toCompletableFuture().join();
        } finally {
            sessionActors.exitRelayDispatch();
        }
    }

    @ZLinkHandlerGroup("play-channel")
    public static final class EnsureActorHandler
            implements ZLinkRequestHandler<EnsureActorRequest, String> {
        @Override
        public CompletionStage<String> handle(
                EnsureActorRequest request, ZLinkMessageContext context) {
            return channelActors
                    .get()
                    .getOrCreate(request.actorId(), "player")
                    .submit()
                    .thenApply(
                            result ->
                                    switch (result) {
                                        case ZLinkActorCreateResult.Created created ->
                                                created.actor().actorId();
                                        case ZLinkActorCreateResult.Existing existing ->
                                                existing.actor().actorId();
                                        case ZLinkActorCreateResult.Rejected rejected ->
                                                throw new IllegalStateException(
                                                        "actor creation rejected: "
                                                                + rejected.reply());
                                    });
        }
    }

    public static final class GameSession implements ZLinkSession {
        private final ZLinkSessionContext context;

        public GameSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            connectedSessions.offer(context.routingId().orElseThrow());
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

    /** An Entry Spot that accepts every Actor type, so the node can create new Actors. */
    public static final class ActorCreationEntry
            implements systems.zlink.framework.spots.ZLinkEntrySpot<
                    systems.zlink.framework.actors.ZLinkActor> {
        private final systems.zlink.framework.spots.ZLinkEntrySpotContext context;

        public ActorCreationEntry(systems.zlink.framework.spots.ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public systems.zlink.framework.spots.ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> onJoinedActor(
                systems.zlink.framework.actors.ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        @Override
        public java.util.concurrent.CompletionStage<Void> onLeaveActor(
                systems.zlink.framework.actors.ZLinkActor actor) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
    }
}
