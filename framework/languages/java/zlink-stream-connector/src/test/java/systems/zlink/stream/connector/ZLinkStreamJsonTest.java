package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.net.URI;
import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRefSnapshot;

final class ZLinkStreamJsonTest {
    @Test
    void frameworkActorReferencesRoundTripThroughDefaultJsonCodec() {
        ActorRefSnapshot expected = new ActorRefSnapshot(
            "courier-a",
            7L,
            "courier-mesh",
            RoutingId.from(new byte[] {0, 65, 66}));

        ActorRefSnapshot actual = ZLinkStreamJson.decode(
            ZLinkStreamJson.encode("actor-ref", expected),
            ActorRefSnapshot.class);

        assertEquals(expected, actual);
    }

    @Test
    void javaTimeValuesRoundTripThroughDefaultJsonCodec() {
        Instant expected = Instant.parse("2026-08-03T05:00:00.123456Z");

        TimestampedPayload actual = ZLinkStreamJson.decode(
            ZLinkStreamJson.encode("timestamped", new TimestampedPayload(expected)),
            TimestampedPayload.class);

        assertEquals(expected, actual.occurredAt());
    }

    @Test
    void typedSendUsesPacketNameAnnotationByDefault() {
        FakeConnector connector = new FakeConnector(options());

        ZLinkStreamJson.send(connector, new AnnotatedPayload("hello"));

        assertEquals("custom.packet", connector.sent.packetName());
        assertEquals(ZLinkStreamCodec.JSON, connector.sent.codec());
    }

    @Test
    void typedOnUsesConnectorNameResolver() {
        FakeConnector connector = new FakeConnector(new ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:1"),
            ZLinkStreamDispatchMode.MANUAL,
            Duration.ofSeconds(1),
            1,
            Duration.ofSeconds(1),
            64 * 1024,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            payloadType -> "resolved." + payloadType.getSimpleName()));

        ZLinkStreamJson.on(
            connector,
            AnnotatedPayload.class,
            message -> CompletableFuture.completedFuture(null));

        assertEquals("resolved.AnnotatedPayload", connector.handlerName);
    }

    @Test
    void typedWaitResolvesPayload() throws Exception {
        FakeConnector connector = new FakeConnector(optionsWithCodec());

        CompletionStage<ZLinkStreamMessage<AnnotatedPayload>> pending =
            connector.waitFor("custom.packet")
                .timeout(Duration.ofSeconds(1))
                .submit(AnnotatedPayload.class);

        connector.handler.handleAsync(new ZLinkStreamMessage<>(
            "custom.packet",
            ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("match")),
            Map.of())).toCompletableFuture().join();

        assertEquals("match", pending.toCompletableFuture().get().payload().value());
    }

    @Test
    void typedWaitUsesConnectorRequestTimeoutByDefault() throws Exception {
        FakeConnector connector = new FakeConnector(optionsWithCodec());

        CompletionStage<ZLinkStreamMessage<AnnotatedPayload>> pending =
            connector.waitFor("custom.packet")
                .submit(AnnotatedPayload.class);

        connector.handler.handleAsync(new ZLinkStreamMessage<>(
            "custom.packet",
            ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("match")),
            Map.of())).toCompletableFuture().join();

        assertEquals("match", pending.toCompletableFuture().get().payload().value());
    }

    @Test
    void encodedRequestCanDecodeTypedReplyWithConfiguredCodec() throws Exception {
        FakeConnector connector = new FakeConnector(optionsWithCodec());

        AnnotatedPayload reply = connector
            .request(ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("hello")))
            .submit(AnnotatedPayload.class)
            .toCompletableFuture()
            .get();

        assertEquals("custom.packet", connector.requested.packetName());
        assertEquals(ZLinkStreamCodec.JSON, connector.requested.codec());
        assertEquals("reply", reply.value());
    }

    @Test
    void connectorTypedWaitUsesConfiguredCodec() throws Exception {
        FakeConnector connector = new FakeConnector(optionsWithCodec());

        CompletionStage<ZLinkStreamMessage<AnnotatedPayload>> pending =
            connector.waitFor("custom.packet").submit(AnnotatedPayload.class);

        connector.handler.handleAsync(new ZLinkStreamMessage<>(
            "custom.packet",
            ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("match")),
            Map.of())).toCompletableFuture().join();

        assertEquals("match", pending.toCompletableFuture().get().payload().value());
    }

    @Test
    void connectorTypedWaitWhereFiltersBeforeCompleting() throws Exception {
        FakeConnector connector = new FakeConnector(optionsWithCodec());

        CompletionStage<ZLinkStreamMessage<AnnotatedPayload>> pending =
            connector.waitFor("custom.packet")
                .where(AnnotatedPayload.class, message -> message.payload().value().equals("match"))
                .submit(AnnotatedPayload.class);

        connector.handler.handleAsync(new ZLinkStreamMessage<>(
            "custom.packet",
            ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("skip")),
            Map.of())).toCompletableFuture().join();
        connector.handler.handleAsync(new ZLinkStreamMessage<>(
            "custom.packet",
            ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("match")),
            Map.of())).toCompletableFuture().join();

        assertEquals("match", pending.toCompletableFuture().get().payload().value());
    }

    private static ZLinkStreamConnectorOptions options() {
        return new ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:1"),
            ZLinkStreamDispatchMode.MANUAL,
            Duration.ofSeconds(1),
            1);
    }

    private static ZLinkStreamConnectorOptions optionsWithCodec() {
        return new ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:1"),
            ZLinkStreamDispatchMode.MANUAL,
            Duration.ofSeconds(1),
            1,
            Duration.ofSeconds(1),
            64 * 1024,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            ZLinkStreamJson.codec());
    }

    @ZLinkStreamPacketName("custom.packet")
    private record AnnotatedPayload(String value) {
    }

    private record TimestampedPayload(Instant occurredAt) {
    }

    private static final class FakeConnector implements ZLinkStreamConnector {
        private final ZLinkStreamConnectorOptions options;
        private ZLinkStreamEncodedPayload sent;
        private ZLinkStreamEncodedPayload requested;
        private String handlerName;
        private ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler;

        FakeConnector(ZLinkStreamConnectorOptions options) {
            this.options = options;
        }

        @Override
        public boolean isConnected() {
            return true;
        }

        @Override
        public ZLinkStreamConnectionState state() {
            return ZLinkStreamConnectionState.CONNECTED;
        }

        @Override
        public ZLinkStreamConnectorOptions options() {
            return options;
        }

        @Override
        public int pendingDispatchCount() {
            return 0;
        }

        @Override
        public int receivedCount(String name) {
            return 0;
        }

        @Override
        public ZLinkStreamLifecycleCall connect() {
            return completedLifecycleCall();
        }

        @Override
        public ZLinkStreamLifecycleCall close() {
            return completedLifecycleCall();
        }

        @Override
        public ZLinkStreamLifecycleCall dispatch() {
            return completedLifecycleCall();
        }

        @Override
        public ZLinkStreamSendCall send(ZLinkStreamEncodedPayload payload) {
            sent = payload;
            return new NoopSendCall();
        }

        @Override
        public ZLinkStreamRequestCall request(ZLinkStreamEncodedPayload payload) {
            requested = payload;
            return new NoopRequestCall();
        }

        @Override
        public AutoCloseable on(
            String name,
            ZLinkStreamMessageHandler<ZLinkStreamEncodedPayload> handler) {
            handlerName = name;
            this.handler = handler;
            return () -> { };
        }

        @Override
        public AutoCloseable observeInbound(ZLinkStreamInboundObserver observer) {
            return () -> { };
        }

        @Override
        public AutoCloseable onErrorReceived(ZLinkStreamErrorHandler handler) {
            return () -> { };
        }

        @Override
        public AutoCloseable onDisconnected(ZLinkStreamDisconnectedHandler handler) {
            return () -> { };
        }

        @Override
        public AutoCloseable onConnectionStateChanged(ZLinkStreamConnectionStateHandler handler) {
            return () -> { };
        }

        private static ZLinkStreamLifecycleCall completedLifecycleCall() {
            return () -> CompletableFuture.completedFuture(null);
        }
    }

    private static final class NoopSendCall implements ZLinkStreamSendCall {
        @Override
        public ZLinkStreamSendCall packetName(String name) {
            return this;
        }

        @Override
        public ZLinkStreamSendCall metadata(String key, String value) {
            return this;
        }

        @Override
        public ZLinkStreamSendCall metadata(Map<String, String> metadata) {
            return this;
        }

        @Override
        public ZLinkStreamSendCall compress() {
            return this;
        }

        @Override
        public CompletionStage<Void> submit() {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class NoopRequestCall implements ZLinkStreamRequestCall {
        @Override
        public ZLinkStreamRequestCall packetName(String name) {
            return this;
        }

        @Override
        public ZLinkStreamRequestCall metadata(String key, String value) {
            return this;
        }

        @Override
        public ZLinkStreamRequestCall metadata(Map<String, String> metadata) {
            return this;
        }

        @Override
        public ZLinkStreamRequestCall compress() {
            return this;
        }

        @Override
        public ZLinkStreamRequestCall timeout(Duration timeout) {
            return this;
        }

        @Override
        public CompletionStage<ZLinkStreamEncodedPayload> submit() {
            return CompletableFuture.completedFuture(
                ZLinkStreamJson.encode("custom.packet", new AnnotatedPayload("reply")));
        }

        @Override
        public <TReply> CompletionStage<TReply> submit(Class<TReply> replyType) {
            return submit().thenApply(reply -> ZLinkStreamJson.decode(reply, replyType));
        }
    }
}
