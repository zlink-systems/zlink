package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.messaging.Message;

import java.net.URI;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.Map;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

class ZLinkStreamStaleDisconnectTest {
    private static final long WAIT_SECONDS = 5;

    @ParameterizedTest
    @CsvSource({
        "ERROR_CALLBACK,true",
        "ERROR_CALLBACK,false",
        "TRANSPORT_CLOSE,true",
        "TRANSPORT_CLOSE,false"
    })
    void oldDisconnectOnlyFailsRequestsOnItsConnection(Window window, boolean replyBeforeDisconnect)
            throws Exception {
        var ending = new EndingGate();
        var first = new ScriptedTransport(window == Window.TRANSPORT_CLOSE ? ending : null);
        var second = new ScriptedTransport(null);
        Queue<ScriptedTransport> transports = new ArrayDeque<>();
        transports.add(first);
        transports.add(second);
        ZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        options(), () -> CompletableFuture.completedFuture(transports.remove()));
        CompletableFuture<ZLinkStreamCloseReason> disconnected = new CompletableFuture<>();
        connector.onDisconnected(
                event -> {
                    disconnected.complete(event.closeReason());
                    return CompletableFuture.completedFuture(null);
                });
        if (window == Window.ERROR_CALLBACK) {
            holdErrorCallback(connector, ending);
        }

        try (var receiver = Executors.newSingleThreadExecutor()) {
            try {
                await(connector.connect().submit());
                var oldRequest = request(connector, "OldRequest");
                assertEquals("OldRequest", await(first.written).name());

                var failure = receiver.submit(first::deliverInvalidHeader);
                await(ending.entered);
                assertFalse(connector.isConnected());

                await(connector.connect().submit());
                var newRequest = request(connector, "NewRequest");
                var sent = await(second.written);
                assertEquals("NewRequest", sent.name());
                if (replyBeforeDisconnect) {
                    second.reply(sent.requestSeq());
                    assertReply(newRequest);
                }

                ending.release.complete(null);
                failure.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(ZLinkStreamCloseReason.PROTOCOL_ERROR, await(disconnected));
                assertDisconnected(oldRequest);

                if (!replyBeforeDisconnect) {
                    second.reply(sent.requestSeq());
                    assertReply(newRequest);
                }
                assertTrue(connector.isConnected());
            } finally {
                ending.release.complete(null);
                await(connector.close().submit());
            }
        }
    }

    @Test
    void connectorCloseFailsPendingOnAnAlreadyDetachedTransport() throws Exception {
        var ending = new EndingGate();
        var transport = new ScriptedTransport(null);
        ZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        options(), () -> CompletableFuture.completedFuture(transport));
        holdErrorCallback(connector, ending);

        try (var receiver = Executors.newSingleThreadExecutor()) {
            try {
                await(connector.connect().submit());
                var pending = request(connector, "Pending");
                await(transport.written);
                receiver.submit(transport::deliverInvalidHeader);
                await(ending.entered);

                await(connector.close().submit());

                assertDisconnected(pending);
            } finally {
                ending.release.complete(null);
                await(connector.close().submit());
            }
        }
    }

    private static void holdErrorCallback(ZLinkStreamConnector connector, EndingGate ending) {
        connector.onErrorReceived(
                error -> {
                    if (error.code() == ZLinkStreamErrorCode.FRAME_DECODE_FAILED) {
                        ending.hold();
                    }
                    return CompletableFuture.completedFuture(null);
                });
    }

    private static void assertDisconnected(CompletionStage<?> request) {
        var failure = assertThrows(ExecutionException.class, () -> await(request));
        assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                assertInstanceOf(ZLinkStreamException.class, failure.getCause()).errorCode());
    }

    private static void assertReply(CompletionStage<ZLinkStreamEncodedPayload> request)
            throws Exception {
        try (var payload = await(request).payload()) {
            assertArrayEquals(new byte[] {7}, payload.toByteArray());
        }
    }

    private static CompletionStage<ZLinkStreamEncodedPayload> request(
            ZLinkStreamConnector connector, String name) {
        try (var payload = Message.from("request")) {
            return connector
                    .request(new ZLinkStreamEncodedPayload(name, payload, Map.of()))
                    .submit();
        }
    }

    private static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
    }

    private static ZLinkStreamConnectorOptions options() {
        return new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:1"),
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

    private enum Window {
        ERROR_CALLBACK,
        TRANSPORT_CLOSE
    }

    private static final class EndingGate {
        private final CompletableFuture<Void> entered = new CompletableFuture<>();
        private final CompletableFuture<Void> release = new CompletableFuture<>();

        void hold() {
            entered.complete(null);
            release.join();
        }
    }

    private static final class ScriptedTransport implements ZLinkStreamTransportConnection {
        private final EndingGate closeGate;
        private final CompletableFuture<ZLinkStreamWireProtocol.Header> written =
                new CompletableFuture<>();
        private CompletableFuture<ZLinkStreamWireProtocol.Frame> inbound =
                new CompletableFuture<>();
        private volatile boolean open = true;

        ScriptedTransport(EndingGate closeGate) {
            this.closeGate = closeGate;
        }

        @Override
        public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
            return inbound;
        }

        @Override
        public CompletionStage<Void> writeAsync(byte[] frame) {
            written.complete(
                    ZLinkStreamWireProtocol.decodeHeader(
                            ZLinkStreamWireProtocol.decodeFrame(frame).header()));
            return CompletableFuture.completedFuture(null);
        }

        void deliverInvalidHeader() {
            deliver(new ZLinkStreamWireProtocol.Frame(new byte[] {-1, -1, -1, -1}, new byte[0]));
        }

        void reply(long requestSeq) {
            byte[] header =
                    ZLinkStreamWireProtocol.encodeHeader(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_RESPONSE,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
                                    requestSeq,
                                    "",
                                    Map.of(),
                                    null));
            deliver(new ZLinkStreamWireProtocol.Frame(header, new byte[] {7}));
        }

        private void deliver(ZLinkStreamWireProtocol.Frame frame) {
            var read = inbound;
            inbound = new CompletableFuture<>();
            assertTrue(read.complete(frame));
        }

        @Override
        public boolean isOpen() {
            return open;
        }

        @Override
        public void close() {
            if (closeGate != null) {
                closeGate.hold();
            }
            open = false;
        }
    }
}
