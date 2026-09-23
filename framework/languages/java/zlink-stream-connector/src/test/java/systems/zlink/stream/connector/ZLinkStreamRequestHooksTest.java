package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;

final class ZLinkStreamRequestHooksTest {
    @Test
    void sendingHooksRunInOrderBeforeFrameAndReplyHookUsesManualDispatch() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                List<String> order = new ArrayList<>();
                List<ZLinkStreamReplyReceivedContext> outcomes = new ArrayList<>();
                Thread caller = Thread.currentThread();
                AutoCloseable first =
                        connector.onRequestSending(
                                context -> {
                                    assertSame(caller, Thread.currentThread());
                                    order.add("first");
                                    assertEquals("Get", context.requestPacketName());
                                    assertNull(context.actorId());
                                    context.setMetadata("one", "1");
                                });
                connector.onRequestSending(
                        context -> {
                            order.add("second");
                            context.setMetadata("two", "2");
                        });
                connector.onReplyReceived(outcomes::add);
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector
                                .request(
                                        new ZLinkStreamEncodedPayload(
                                                "Get", Message.from("hi"), Map.of()))
                                .submit()
                                .toCompletableFuture();
                TcpStreamConnectorTestServer.ReceivedFrame sent = server.readFrameAsync().join();
                assertEquals(List.of("first", "second"), order);
                assertEquals(Map.of("one", "1", "two", "2"), sent.header().metadata());
                assertNull(sent.header().flowId());
                server.sendAsync(
                                TcpStreamConnectorTestServer.responseTo(sent, "", Map.of()),
                                TcpStreamConnectorTestServer.bytes("ok"))
                        .join();
                assertEquals("ok", new String(request.join().payload().toByteArray()));
                assertTrue(outcomes.isEmpty());
                List<ZLinkStreamReplyReceivedContext> late = new ArrayList<>();
                connector.onReplyReceived(late::add);
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.pendingDispatchCount() > 0);
                ConnectorTestAwait.await(connector.dispatch());
                assertEquals(1, outcomes.size());
                assertTrue(late.isEmpty());
                assertTrue(outcomes.getFirst().succeeded());
                assertEquals("Get", outcomes.getFirst().requestPacketName());
                assertEquals(
                        "ok",
                        new String(outcomes.getFirst().reply().payload().payload().toByteArray()));
                outcomes.getFirst().reply().payload().payload().close();
                assertNull(outcomes.getFirst().error());
                assertFalse(outcomes.getFirst().elapsed().isNegative());
                first.close();
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void throwingSendingHookReportsErrorWithoutChangingRequestResult() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                List<ZLinkStreamError> errors = new java.util.concurrent.CopyOnWriteArrayList<>();
                connector.onErrorReceived(
                        error -> {
                            errors.add(error);
                            return CompletableFuture.completedFuture(null);
                        });
                connector.onRequestSending(
                        context -> {
                            context.setMetadata("", "invalid");
                            throw new IllegalStateException("boom");
                        });
                connector.onReplyReceived(
                        context -> {
                            throw new IllegalStateException("reply hook failed");
                        });
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        connector
                                .request(
                                        new ZLinkStreamEncodedPayload(
                                                "Get", Message.from("hi"), Map.of()))
                                .submit()
                                .toCompletableFuture();
                TcpStreamConnectorTestServer.ReceivedFrame sent = server.readFrameAsync().join();
                assertEquals(Map.of(), sent.header().metadata());
                server.sendAsync(
                                TcpStreamConnectorTestServer.responseTo(sent, "", Map.of()),
                                TcpStreamConnectorTestServer.bytes("ok"))
                        .join();
                assertEquals("ok", new String(request.join().payload().toByteArray()));
                TcpStreamConnectorTestServer.awaitCondition(() -> errors.size() == 2);
                assertEquals(2, errors.size());
                assertTrue(
                        errors.stream()
                                .allMatch(
                                        error ->
                                                error.code()
                                                        == ZLinkStreamErrorCode
                                                                .USER_CALLBACK_FAILED));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void actorRequestAndRemoteErrorReportActorIdAndCodedFailure() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                List<ZLinkStreamRequestSendingContext> sending = new ArrayList<>();
                List<ZLinkStreamReplyReceivedContext> outcomes = new ArrayList<>();
                connector.onRequestSending(sending::add);
                connector.onReplyReceived(outcomes::add);
                ConnectorTestAwait.await(connector.connect());
                byte[] id = "actor-a".getBytes(StandardCharsets.UTF_8);
                byte[] bound =
                        ByteBuffer.allocate(4 + id.length)
                                .put((byte) 1)
                                .putShort((short) 7)
                                .put((byte) id.length)
                                .put(id)
                                .array();
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_CONTROL,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        0,
                                        null,
                                        ZLinkStreamActorRegistry.BOUND,
                                        Map.of(),
                                        null),
                                bound)
                        .join();
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.actor("actor-a").isPresent());
                ZLinkStreamActor actor = connector.actor("actor-a").orElseThrow();
                CompletableFuture<ZLinkStreamEncodedPayload> request =
                        actor.request(
                                        new ZLinkStreamEncodedPayload(
                                                "Get", Message.from("hi"), Map.of()))
                                .submit()
                                .toCompletableFuture();
                TcpStreamConnectorTestServer.ReceivedFrame sent = server.readFrameAsync().join();
                assertEquals("actor-a", sending.getFirst().actorId());
                assertEquals(7, sent.header().actorSlot());
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
                                        "{\"code\":\"denied\",\"message\":\"no\"}"))
                        .join();
                assertTrue(
                        org.junit.jupiter.api.Assertions.assertThrows(
                                                CompletionException.class, request::join)
                                        .getCause()
                                instanceof ZLinkStreamException);
                TcpStreamConnectorTestServer.awaitCondition(() -> outcomes.size() == 1);
                assertEquals("actor-a", outcomes.getFirst().actorId());
                assertFalse(outcomes.getFirst().succeeded());
                assertNull(outcomes.getFirst().reply());
                assertEquals(ZLinkStreamErrorCode.REMOTE_ERROR, outcomes.getFirst().error().code());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }
}
