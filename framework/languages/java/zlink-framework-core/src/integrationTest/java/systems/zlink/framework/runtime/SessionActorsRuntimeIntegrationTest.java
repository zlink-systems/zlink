package systems.zlink.framework.runtime;

import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.net.ServerSocket;
import java.nio.charset.StandardCharsets;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.handlers.ZLinkRequest;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionActor;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkStreamError;

final class SessionActorsRuntimeIntegrationTest {
    static final LinkedBlockingQueue<String> actorRelayRequests =
        new LinkedBlockingQueue<>();
    static final AtomicReference<ZLinkActorManager> channelActors =
        new AtomicReference<>();

    @Test
    void bindUsesStreamActorGatewayBindingPath() {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime()) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            ZLinkSessionActorsRuntime sessionActors = runtime.sessionActors(
                "gateway",
                RoutingId.from("session-1"));
            ZLinkSessionActor bound = sessionActors
                .bind(actor)
                .toCompletableFuture()
                .join();

            assertEquals("player-1", bound.actorId());
            assertEquals(Optional.of(bound), sessionActors.find("player-1"));
        }
    }

    @Test
    void bindCanRelayLocalManagedActorWithoutActorGatewayAttach() throws Exception {
        actorRelayRequests.clear();
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startLocalManagedStreamRuntime()) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            ZLinkSessionActor bound = runtime.sessionActors(
                    "local",
                    RoutingId.from("session-1"))
                .bind(actor)
                .toCompletableFuture()
                .join();

            relayWithHeader(bound, "ActorNotify", ZLinkMessage.of(new ActorNotifyMessage("hello")));

            assertEquals(
                "player-1:hello",
                awaitActorRelay("player-1:hello", 2, TimeUnit.SECONDS));
        }
    }

    @Test
    void channelRequestHandler_canCreateActorAndJoinEntrySpot() {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startChannelJoinRuntime()) {
            channelActors.set(runtime.actorManager());
            String actorId = runtime.client()
                .requestToChannel("play", new EnsureActorRequest("player-1"))
                .timeout(Duration.ofSeconds(2))
                .submit(String.class)
                .toCompletableFuture()
                .join();

            assertEquals("player-1", actorId);
        }
    }

    @Test
    void sessionAndPlayServers_relaySucceeds() {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime()) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            ZLinkSessionActorsRuntime sessionActors = runtime.sessionActors(
                "gateway",
                RoutingId.from("session-1"));
            ZLinkSessionActor bound = sessionActors
                .bind(actor)
                .toCompletableFuture()
                .join();

            assertEquals("player-1", bound.actorId());
            assertEquals(Optional.of(bound), sessionActors.find("player-1"));
        }
    }

    @Test
    void playActorPush_withoutLiveClientStreamFailsNativeSend() {
        Zlink.version();
        try (ZLinkFrameworkRuntime runtime = startGatewayRuntime()) {
            ZLinkActor actor = managedActor(runtime, "player-1", "player");
            runtime.sessionActors("gateway", RoutingId.from("session-1"))
                .bind(actor)
                .toCompletableFuture()
                .join();

            actor.context().boundSession().send("push").submit();
        }
    }

    private static ZLinkActor managedActor(
        ZLinkFrameworkRuntime runtime,
        String actorId,
        String actorType) {
        return ((ZLinkActorRuntime) runtime.actorManager())
            .getOrCreateManagedActor(actorId, actorType)
            .toCompletableFuture()
            .join();
    }

    private static ZLinkFrameworkRuntime startGatewayRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://play-" + System.nanoTime()).setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation());
                node.objects().server().addEntrySpot(GameEntrySpot.class); node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.disableRelocation()); }
        { var stream = options.addStreamNode("gateway"); stream.bind("inproc://gateway-bind-" + System.nanoTime());
            stream.enableActorDispatch();
            stream.registerSession(GameSession.class); };

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startLocalManagedStreamRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://local-play-" + System.nanoTime()).setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addEntrySpot(GameEntrySpot.class);
                node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation()); node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.disableRelocation()); }
        { var stream = options.addStreamNode("local"); stream.bind("inproc://local-managed-bind-" + System.nanoTime());
            stream.enableActorDispatch();
            stream.registerSession(GameSession.class); };

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startBoundGatewayRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        String suffix = Long.toString(System.nanoTime());
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://play-router-" + suffix)
                    .setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation());
                node.objects().server().addEntrySpot(GameEntrySpot.class); node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.disableRelocation()); }

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    private static ZLinkFrameworkRuntime startChannelJoinRuntime() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var channel = options.addClientServerChannel("play").server().listen();
            options.addClientServerChannel("play").client();
            channel.addRequestHandler(
                EnsureActorHandler.class,
                EnsureActorRequest.class,
                String.class); };
        { var node = options.addRouteMesh("game"); node.listen("inproc://channel-play-" + System.nanoTime()).setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addEntrySpot(GameEntrySpot.class); node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.disableRelocation()); }

        return RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
    }

    static String tcpEndpoint() throws Exception {
        try (ServerSocket server = new ServerSocket(0)) {
            return "tcp://127.0.0.1:" + server.getLocalPort();
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
            context.handlers().addHandler(ActorNotifyHandler.class);
        }

        @Override public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
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
            context.handlers().addHandler(ActorNotifyHandler.class);
        }

        @Override public CompletionStage<Void> onJoinedActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
        }
        @Override public CompletionStage<Void> onLeaveActor(PlayerActor actor) {
            return CompletableFuture.completedFuture(null);
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

    public record JsonRelayReq(String value) {
    }

    public record JsonRelayRes(String value) {
    }

    @ZLinkPacket("ActorEcho")
    public record ActorEchoRequest(String value) {
    }

    @ZLinkPacket("ActorNotify")
    public record ActorNotifyMessage(String value) {
    }

    @ZLinkPacket("Ensure")
    public record EnsureActorRequest(String actorId) {
    }

    public static final class DefaultJsonActorHandler {
        @ZLinkSpotActorRequest
        public CompletionStage<JsonRelayRes> handle(PlayerActor actor, JsonRelayReq request) {
            actorRelayRequests.offer(actor.context().actorId() + ":" + request.value());
            return CompletableFuture.completedFuture(new JsonRelayRes(request.value()));
        }
    }

    private static String awaitActorRelay(
        String expected,
        long timeout,
        TimeUnit unit) throws InterruptedException, java.util.concurrent.TimeoutException {
        long deadline = System.nanoTime() + unit.toNanos(timeout);
        while (true) {
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                throw new java.util.concurrent.TimeoutException();
            }
            String received = actorRelayRequests.poll(remaining, TimeUnit.NANOSECONDS);
            if (expected.equals(received)) {
                return received;
            }
        }
    }

    static String relayUntilActorReceived(
        ZLinkSessionActor bound,
        byte[] payloadBytes,
        String expected,
        long timeout,
        TimeUnit unit) throws Exception {
        long deadline = System.nanoTime() + unit.toNanos(timeout);
        while (true) {
            relayWithHeader(
                bound,
                "ActorNotify",
                ZLinkMessage.of(new ActorNotifyMessage(
                    new String(payloadBytes, StandardCharsets.UTF_8))));
            long remaining = deadline - System.nanoTime();
            if (remaining <= 0) {
                throw new java.util.concurrent.TimeoutException();
            }
            String received = actorRelayRequests.poll(
                Math.min(remaining, TimeUnit.MILLISECONDS.toNanos(100)),
                TimeUnit.NANOSECONDS);
            if (expected.equals(received)) {
                return received;
            }
        }
    }

    static String uniqueActorId(String prefix) {
        return prefix + "-" + Long.toUnsignedString(System.nanoTime(), 36);
    }

    static void relayWithHeader(
        ZLinkSessionActor actor,
        String packetName,
        ZLinkMessage payload) {
        ZLinkSessionActorsRuntime.enterRelayDispatch(
            new ZLinkStreamHeader(packetName, java.util.Map.of(), Optional.empty()));
        try {
            actor.relay(payload).toCompletableFuture().join();
        } finally {
            ZLinkSessionActorsRuntime.exitRelayDispatch();
        }
    }

    @ZLinkHandlerGroup("play-channel")
    public static final class EnsureActorHandler
        implements systems.zlink.framework.channels.ZLinkRequestHandler<
            EnsureActorRequest,
            String> {
        @Override
        public CompletionStage<String> handle(
            EnsureActorRequest request,
            ZLinkMessageContext context) {
            return channelActors.get().getOrCreate(request.actorId(), "player")
                .submit()
                .thenApply(result -> switch (result) {
                    case systems.zlink.framework.actors.ZLinkActorCreateResult.Created created ->
                        created.actor().actorId();
                    case systems.zlink.framework.actors.ZLinkActorCreateResult.Existing existing ->
                        existing.actor().actorId();
                    case systems.zlink.framework.actors.ZLinkActorCreateResult.Rejected rejected ->
                        throw new IllegalStateException(
                            "actor creation rejected: " + rejected.reply());
                });
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
}
