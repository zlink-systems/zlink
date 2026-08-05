package systems.zlink.framework.runtime;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;

import systems.zlink.framework.runtime.internal.backend.*;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.handlers.ZLinkPacket;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorManager;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;

final class StreamSessionTest {
    @Test
    void streamNodeStartsWithoutExplicitSessionRelayAttach() {
        Zlink.version();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://play-router-" + System.nanoTime());
                node.objects().server().addSpotFactory("GameSpot", GameSpot.class, factory -> factory.disableRelocation()); }
        { var stream = options.addStreamNode("gateway"); stream.bind("inproc://gateway-" + System.nanoTime());
            stream.registerSession(GameSession.class); };

        try (ZLinkFrameworkRuntime ignored =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory())) {
        }
    }

    @Test
    void streamNodeDispatchesTcpRequestAndReplies() throws Exception {
        Zlink.version();
        EchoSession.reset();
        int port = reservePort();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var stream = options.addStreamNode("gateway"); stream.bind("tcp://127.0.0.1:" + port);
            stream.registerSession(EchoSession.class); };

        try (ZLinkFrameworkRuntime ignored =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
             Socket client = new Socket("127.0.0.1", port)) {
            client.setSoTimeout(3000);
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
        int port = reservePort();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        { var stream = options.addStreamNode("gateway"); stream.bind("tcp://127.0.0.1:" + port);
            stream.registerSession(RecoveringSession.class); };

        try (ZLinkFrameworkRuntime ignored =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
             Socket client = new Socket("127.0.0.1", port)) {
            client.setSoTimeout(3000);

            client.getOutputStream().write(frame(requestHeader(11L, "MustFail"), bytes("bad")));
            client.getOutputStream().flush();
            assertErrorReply(
                client.getInputStream(),
                11L,
                "IllegalStateException",
                "public failure");

            client.getOutputStream().write(frame(requestHeader(12L, "Ping"), bytes("again")));
            client.getOutputStream().flush();
            assertReply(client.getInputStream(), 12L, "\"pong\"");
        }
    }

    @Test
    void streamActorGatewayRelaysRequestAndReplies() throws Exception {
        Zlink.version();
        int port = reservePort();
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.addHandlersFromPackageOf(StreamSessionTest.class);
        options.addLocationStore(new ZLinkInMemoryLocationStore());
        { var node = options.addRouteMesh("game"); node.listen("inproc://stream-play-" + System.nanoTime()).setRoutingId(RoutingId.from("play-node"));
                node.objects().server().addEntrySpot(GameEntrySpot.class); node.objects().server().addActorFactory("player", PlayerActor.class, PlayerActorFactory.class, factory -> factory.disableRelocation()); }
        { var stream = options.addStreamNode("gateway"); stream.bind("tcp://127.0.0.1:" + port);
            stream.enableActorDispatch();
            stream.registerSession(ActorRelaySession.class); };

        try (ZLinkFrameworkRuntime ignored =
                 RuntimeTestSupport.startFramework(options, new ZLinkJavaBackendAdapterFactory());
             Socket client = new Socket("127.0.0.1", port)) {
            client.setSoTimeout(3000);

            client.getOutputStream().write(frame(requestHeader(1L, "Bind"), bytes("\"player-1\"")));
            client.getOutputStream().flush();
            assertReply(client.getInputStream(), 1L, "\"bound\"");

            client.getOutputStream().write(frame(requestHeader(2L, "StreamActorEcho"), bytes("\"hello\"")));
            client.getOutputStream().flush();
            assertReply(client.getInputStream(), 2L, "\"player-1:hello\"");
        }
    }

    public static final class GameSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
    }

    public static final class GameEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
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

        @Override public CompletionStage<Void> onJoinedActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
        @Override public CompletionStage<Void> onLeaveActor(ZLinkActor actor) { return CompletableFuture.completedFuture(null); }
    }

    public static final class UserSpot implements ZLinkSpot<ZLinkActor> {
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
            String actorId,
            ZLinkMessage request) {
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
        @ZLinkSpotActorRequest(packetName = "StreamActorEcho")
        public CompletionStage<String> handle(PlayerActor actor, String request) {
            return CompletableFuture.completedFuture(
                actor.context().actorId() + ":" + request);
        }

        @ZLinkSpotActorRequest(packetName = "StreamActorEchoWithPush")
        public CompletionStage<String> handleWithPush(PlayerActor actor, String request) {
            actor.context()
                .boundSession()
                .send(new StreamActorPush("push:" + request))
                .submit();
            return CompletableFuture.completedFuture(
                actor.context().actorId() + ":" + request);
        }
    }

    @ZLinkPacket("StreamActorPush")
    public record StreamActorPush(String value) {
    }

    @ZLinkPacket("JoinUserSpot")
    public record JoinUserSpotRequest(String value) {
    }

    public static final class JoinUserSpotHandler implements
        ZLinkEntrySpotActorRequestHandler<
            GameEntrySpot,
            PlayerActor,
            JoinUserSpotRequest,
            String> {
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
            UserSpot spot,
            PlayerActor actor,
            ZLinkMessageContext context,
            String request) {
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
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
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
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
            if ("MustFail".equals(dispatch.packetName())) {
                throw new IllegalStateException("public failure");
            }
            recovered.set(true);
            context.client()
                .reply("pong")
                .submit();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ActorRelaySession implements ZLinkSession {
        private final ZLinkSessionContext context;
        private final ZLinkActorManager actors;

        public ActorRelaySession(
            ZLinkSessionContext context,
            ZLinkActorManager actors) {
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
            ZLinkSessionDispatchContext dispatch,
            ZLinkMessage payload) {
            if ("Bind".equals(dispatch.packetName())) {
                String actorId = payload.decode(String.class);
                return actors.getOrCreate(actorId, "player")
                    .submit()
                    .thenCompose(result -> {
                        var actor = switch (result) {
                            case systems.zlink.framework.actors.ZLinkActorCreateResult.Created created ->
                                created.actor();
                            case systems.zlink.framework.actors.ZLinkActorCreateResult.Existing existing ->
                                existing.actor();
                            case systems.zlink.framework.actors.ZLinkActorCreateResult.Rejected rejected ->
                                throw new IllegalStateException(
                                    "actor creation rejected: " + rejected.reply());
                        };
                        return context.actors().bind(actor);
                    })
                    .thenRun(() -> context.client().reply("bound").submit());
            }
            return context.actors().bound().get(0).relay(payload);
        }
    }

    private static int reservePort() throws Exception {
        try (ServerSocket server = new ServerSocket(0)) {
            return server.getLocalPort();
        }
    }

    private static byte[] requestHeader(long requestSeq, String packetName) {
        return ZLinkStreamHeaderCodec.encode(new ZLinkStreamHeader(
            systems.zlink.framework.streams.ZLinkStreamMessageKind.REQUEST,
            systems.zlink.framework.streams.ZLinkStreamCodec.RAW,
            java.util.EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            java.util.Optional.of(requestSeq),
            packetName,
            java.util.Map.of(),
            java.util.Optional.empty()));
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

    private static void assertReply(InputStream input, long requestSeq, String expectedBody)
        throws Exception {
        byte[] prefix = input.readNBytes(6);
        assertEquals(6, prefix.length);
        ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
        int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
        int bodySize = prefixBuffer.getInt();
        byte[] header = readExact(input, headerSize);
        byte[] body = readExact(input, bodySize);

        assertEquals(
            3,
            Byte.toUnsignedInt(header[1]),
            () -> "unexpected frame kind body=" + new String(body, StandardCharsets.UTF_8));
        assertEquals(0xF2, Byte.toUnsignedInt(header[0]));
        assertEquals(requestSeq, ByteBuffer.wrap(header, 4, Long.BYTES).getLong());
        assertEquals(expectedBody, new String(body, StandardCharsets.UTF_8));
    }

    private static void assertErrorReply(
        InputStream input,
        long requestSeq,
        String expectedCode,
        String expectedMessage)
        throws Exception {
        byte[] prefix = input.readNBytes(6);
        assertEquals(6, prefix.length);
        ByteBuffer prefixBuffer = ByteBuffer.wrap(prefix);
        int headerSize = Short.toUnsignedInt(prefixBuffer.getShort());
        int bodySize = prefixBuffer.getInt();
        byte[] header = readExact(input, headerSize);
        byte[] body = readExact(input, bodySize);

        if (Byte.toUnsignedInt(header[1]) == 5) {
            assertErrorReply(
                input, requestSeq, expectedCode, expectedMessage);
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

    private static void assertSend(InputStream input, String expectedPacketName, String expectedBody)
        throws Exception {
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
        String packetName = new String(
            header,
            nameLengthOffset + 1,
            nameLength,
            StandardCharsets.UTF_8);
        assertEquals(expectedPacketName, packetName);
        assertEquals(expectedBody, new String(body, StandardCharsets.UTF_8));
    }

    private static void assertEventually(java.util.function.BooleanSupplier condition)
        throws Exception {
        long deadline = System.nanoTime() + java.util.concurrent.TimeUnit.SECONDS.toNanos(3);
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
