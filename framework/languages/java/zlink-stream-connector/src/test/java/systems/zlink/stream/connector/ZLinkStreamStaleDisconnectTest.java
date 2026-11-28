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
import systems.zlink.stream.connector.ScriptedStreamTransport.EndingGate;

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
        var first = new ScriptedStreamTransport(window == Window.TRANSPORT_CLOSE ? ending : null);
        var second = new ScriptedStreamTransport(null);
        Queue<ScriptedStreamTransport> transports = new ArrayDeque<>();
        transports.add(first);
        transports.add(second);
        ZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        ScriptedStreamTransport.options(),
                        () -> CompletableFuture.completedFuture(transports.remove()));
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
                assertEquals("OldRequest", await(first.written()).name());

                var failure = receiver.submit(first::deliverInvalidHeader);
                ending.awaitEntered(WAIT_SECONDS);
                assertFalse(connector.isConnected());

                await(connector.connect().submit());
                var newRequest = request(connector, "NewRequest");
                var sent = await(second.written());
                assertEquals("NewRequest", sent.name());
                if (replyBeforeDisconnect) {
                    second.reply(sent.requestSeq());
                    assertReply(newRequest);
                }

                ending.release();
                failure.get(WAIT_SECONDS, TimeUnit.SECONDS);
                assertEquals(ZLinkStreamCloseReason.PROTOCOL_ERROR, await(disconnected));
                assertDisconnected(oldRequest);

                if (!replyBeforeDisconnect) {
                    second.reply(sent.requestSeq());
                    assertReply(newRequest);
                }
                assertTrue(connector.isConnected());
            } finally {
                ending.release();
                await(connector.close().submit());
            }
        }
    }

    @Test
    void connectorCloseFailsPendingOnAnAlreadyDetachedTransport() throws Exception {
        var ending = new EndingGate();
        var transport = new ScriptedStreamTransport(null);
        ZLinkStreamConnector connector =
                new DefaultZLinkStreamConnector(
                        ScriptedStreamTransport.options(),
                        () -> CompletableFuture.completedFuture(transport));
        holdErrorCallback(connector, ending);

        try (var receiver = Executors.newSingleThreadExecutor()) {
            try {
                await(connector.connect().submit());
                var pending = request(connector, "Pending");
                await(transport.written());
                receiver.submit(transport::deliverInvalidHeader);
                ending.awaitEntered(WAIT_SECONDS);

                await(connector.close().submit());

                assertDisconnected(pending);
            } finally {
                ending.release();
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

    private enum Window {
        ERROR_CALLBACK,
        TRANSPORT_CLOSE
    }
}
