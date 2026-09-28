package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.messaging.Message;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;

/** Spec 32 5.2, 5.7, 6.2, 9 and 12: how a connection ends and what a caller sees after it. */
final class ZLinkStreamConnectionEndingTest {
    private static final long WAIT_SECONDS = 5;

    /**
     * Spec 32 9, 12: a frame or header decode failure and a frame over the receive limit end the
     * connection with ProtocolError, and a transport read failure with TransportError. The pending
     * request fails with Disconnected whatever ended the connection.
     */
    @ParameterizedTest
    @CsvSource({
        "decode, PROTOCOL_ERROR",
        "tooLarge, PROTOCOL_ERROR",
        "readFailure, TRANSPORT_ERROR"
    })
    void aReceiveFailureEndsTheConnectionWithTheReasonDecidedWhereItOccurs(
            String failure, ZLinkStreamCloseReason expected) throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint()));
            CompletableFuture<ZLinkStreamCloseReason> disconnected = new CompletableFuture<>();
            connector.onDisconnected(
                    event -> {
                        disconnected.complete(event.closeReason());
                        return CompletableFuture.completedFuture(null);
                    });
            try {
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector.request(payload("Pending")).submit().toCompletableFuture();
                server.readFrameAsync().get(WAIT_SECONDS, TimeUnit.SECONDS);

                switch (failure) {
                    case "decode" ->
                            server.sendRawAsync(
                                            new byte[] {(byte) 0xFF, (byte) 0xFF, (byte) 0xFF},
                                            new byte[0])
                                    .join();
                    case "tooLarge" ->
                            server.sendBytesAsync(
                                            ByteBuffer.allocate(6)
                                                    .putShort((short) 0)
                                                    .putInt(64 * 1024 + 1)
                                                    .array())
                                    .join();
                    default -> server.closeCurrentSocket();
                }

                assertEquals(ZLinkStreamErrorCode.DISCONNECTED, codeOf(request));
                assertEquals(expected, disconnected.get(WAIT_SECONDS, TimeUnit.SECONDS));
                assertEquals(Optional.of(expected), connector.closeReason());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * Spec 32 9: a WebSocket message over the receive limit is FrameTooLarge. The error event
     * reports it, the connection ends with ProtocolError, and the pending request fails with
     * Disconnected, not with the code of the cause.
     */
    @Test
    void aWebSocketMessageOverTheLimitIsFrameTooLarge() throws Exception {
        try (WebSocketStreamConnectorTestServer server = new WebSocketStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint()));
            CompletableFuture<ZLinkStreamError> streamError = new CompletableFuture<>();
            connector.onErrorReceived(
                    error -> {
                        streamError.complete(error);
                        return CompletableFuture.completedFuture(null);
                    });
            try {
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector.request(payload("Pending")).submit().toCompletableFuture();

                server.sendRawAsync(new byte[6 + 65535 + 64 * 1024 + 1]).join();

                assertEquals(
                        ZLinkStreamErrorCode.FRAME_TOO_LARGE,
                        streamError.get(WAIT_SECONDS, TimeUnit.SECONDS).code());
                assertEquals(ZLinkStreamErrorCode.DISCONNECTED, codeOf(request));
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.closeReason().isPresent());
                assertEquals(
                        Optional.of(ZLinkStreamCloseReason.PROTOCOL_ERROR),
                        connector.closeReason());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * A WebSocket message within the receive limit is delivered whole, even when the transport
     * hands it over in parts. The spec has no rule against fragmented messages.
     */
    @Test
    void aLargeWebSocketMessageWithinTheLimitIsDelivered() throws Exception {
        int size = 130 * 1024;
        try (WebSocketStreamConnectorTestServer server = new WebSocketStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), 256 * 1024));
            try {
                ConnectorTestAwait.await(connector.connect());
                byte[] body = new byte[size];
                for (int index = 0; index < size; index++) {
                    body[index] = (byte) index;
                }
                byte[] header =
                        ZLinkStreamWireProtocol.encodeHeader(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        0,
                                        null,
                                        "Large",
                                        Map.of(),
                                        null));
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> received =
                        connector
                                .waitFor("Large")
                                .timeout(Duration.ofSeconds(WAIT_SECONDS))
                                .submit()
                                .toCompletableFuture();

                server.sendRawAsync(ZLinkStreamWireProtocol.encodeFrame(header, body, size)).join();

                ZLinkStreamMessage<ZLinkStreamEncodedPayload> message =
                        received.get(WAIT_SECONDS, TimeUnit.SECONDS);
                try {
                    assertArrayEquals(body, message.payload().payload().toByteArray());
                } finally {
                    message.payload().payload().close();
                }
                assertTrue(connector.isConnected());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * Spec 32 4.7, 9: the decompressed payload is compared with the receive limit too. Over it is
     * FrameTooLarge, which ends the connection with ProtocolError; the pending request fails with
     * Disconnected.
     */
    @Test
    void aDecompressedPayloadOverTheReceiveLimitEndsTheConnection() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), 1024));
            CompletableFuture<ZLinkStreamError> streamError = new CompletableFuture<>();
            CompletableFuture<ZLinkStreamCloseReason> disconnected = new CompletableFuture<>();
            connector.onDisconnected(
                    event -> {
                        disconnected.complete(event.closeReason());
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onErrorReceived(
                    error -> {
                        streamError.complete(error);
                        return CompletableFuture.completedFuture(null);
                    });
            try {
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector.request(payload("Pending")).submit().toCompletableFuture();
                server.readFrameAsync().get(WAIT_SECONDS, TimeUnit.SECONDS);

                byte[] compressed = ZLinkStreamLz4Pickler.pickle(new byte[4096]);
                assertTrue(compressed.length <= 1024);
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
                                        null,
                                        "TooLarge",
                                        Map.of(),
                                        null),
                                compressed)
                        .join();

                assertEquals(ZLinkStreamErrorCode.DISCONNECTED, codeOf(request));
                assertEquals(
                        ZLinkStreamErrorCode.FRAME_TOO_LARGE,
                        streamError.get(WAIT_SECONDS, TimeUnit.SECONDS).code());
                assertEquals(
                        ZLinkStreamCloseReason.PROTOCOL_ERROR,
                        disconnected.get(WAIT_SECONDS, TimeUnit.SECONDS));
                assertEquals(
                        Optional.of(ZLinkStreamCloseReason.PROTOCOL_ERROR),
                        connector.closeReason());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * Spec 32 5.2, 5.7 and the Java language spec 11: a Request the caller cancels after its frame
     * was written ends with CancellationException, not ZLinkStreamException, does not run the reply
     * received hook and leaves the pending map, so an Error reply with its sequence reaches the
     * error surface as a stream-level error.
     */
    @Test
    void aCancelledRequestSkipsTheReplyHookAndLeavesThePendingMap() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            new ZLinkStreamConnectorOptions(
                                    server.endpoint(),
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    Duration.ofSeconds(30),
                                    1,
                                    Duration.ofSeconds(5),
                                    64 * 1024,
                                    false,
                                    Duration.ofSeconds(1),
                                    Duration.ofSeconds(5),
                                    false,
                                    Duration.ofMillis(10),
                                    Duration.ofMillis(20),
                                    2.0));
            List<ZLinkStreamReplyReceivedContext> hooks = new CopyOnWriteArrayList<>();
            connector.onReplyReceived(hooks::add);
            CompletableFuture<ZLinkStreamError> streamError = new CompletableFuture<>();
            connector.onErrorReceived(
                    error -> {
                        streamError.complete(error);
                        return CompletableFuture.completedFuture(null);
                    });
            try {
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector.request(payload("Cancelled")).submit().toCompletableFuture();
                TcpStreamConnectorTestServer.ReceivedFrame sent =
                        server.readFrameAsync().get(WAIT_SECONDS, TimeUnit.SECONDS);

                assertTrue(request.cancel(false));
                assertThrows(CancellationException.class, request::join);

                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_ERROR,
                                        ZLinkStreamWireProtocol.CODEC_JSON,
                                        ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                                        sent.header().requestSeq(),
                                        "",
                                        Map.of(),
                                        null),
                                TcpStreamConnectorTestServer.bytes(
                                        "{\"code\":\"late\",\"message\":\"after cancel\"}"))
                        .join();

                assertEquals(
                        ZLinkStreamErrorCode.REMOTE_ERROR,
                        streamError.get(WAIT_SECONDS, TimeUnit.SECONDS).code());
                assertEquals(List.of(), hooks);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * Spec 32 10: a packet stays in the receive queue until a handler or a wait takes it, so a
     * handler registered after the packet arrived receives it. The handlers are decided when the
     * packet is dispatched.
     */
    @ParameterizedTest
    @CsvSource({"MANUAL", "IMMEDIATE"})
    void aHandlerRegisteredAfterThePacketArrivedReceivesIt(ZLinkStreamDispatchMode mode)
            throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(options(server.endpoint(), mode));
            try {
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(push("Late"), TcpStreamConnectorTestServer.bytes("late")).join();
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.receivedCount("Late") == 1);

                CompletableFuture<String> received = new CompletableFuture<>();
                connector.on(
                        "Late",
                        message -> {
                            received.complete(message.packetName());
                            return CompletableFuture.completedFuture(null);
                        });
                ConnectorTestAwait.await(connector.dispatch());

                assertEquals("Late", received.get(WAIT_SECONDS, TimeUnit.SECONDS));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    private static ZLinkStreamWireProtocol.Header push(String name) {
        return new ZLinkStreamWireProtocol.Header(
                ZLinkStreamWireProtocol.KIND_SEND,
                ZLinkStreamWireProtocol.CODEC_RAW,
                0,
                null,
                name,
                Map.of(),
                null);
    }

    private static ZLinkStreamConnectorOptions options(
            java.net.URI endpoint, ZLinkStreamDispatchMode mode) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                mode,
                Duration.ofSeconds(30),
                1,
                Duration.ofSeconds(5),
                64 * 1024,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                false,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0);
    }

    private static ZLinkStreamConnectorOptions options(
            java.net.URI endpoint, int maxReceivePayloadSize) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(30),
                Duration.ofSeconds(5),
                1,
                Duration.ofSeconds(5),
                64 * 1024,
                maxReceivePayloadSize,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                false,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0,
                false,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    private static ZLinkStreamConnectorOptions options(java.net.URI endpoint) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(30),
                1,
                Duration.ofSeconds(5),
                64 * 1024,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                false,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0);
    }

    private static ZLinkStreamEncodedPayload payload(String packetName) {
        return new ZLinkStreamEncodedPayload(packetName, Message.from("x"), Map.of());
    }

    private static ZLinkStreamErrorCode codeOf(CompletableFuture<?> stage) throws Exception {
        CompletionException failure =
                assertThrows(
                        CompletionException.class,
                        () -> stage.orTimeout(WAIT_SECONDS, TimeUnit.SECONDS).join());
        return assertInstanceOf(ZLinkStreamException.class, failure.getCause()).errorCode();
    }
}
