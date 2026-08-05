package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;

final class ZLinkRouteMeshInboundIdentityIntegrationTest {
    @Test
    void nonInitiatorCanRequestBackByInboundProbeIdentityAndRelearnAfterReconnect()
        throws Exception {
        assumeNative();

        RoutingId initiatorRid = RoutingId.from("route-a-initiator");
        RoutingId listenerRid = RoutingId.from("route-z-listener");
        assertConnectLearnsInboundIdentity(initiatorRid, listenerRid, "first");
        assertConnectLearnsInboundIdentity(initiatorRid, listenerRid, "second");
    }

    private static void assertConnectLearnsInboundIdentity(
        RoutingId initiatorRid,
        RoutingId listenerRid,
        String marker)
        throws Exception {
        String endpoint = "inproc://java-route-inbound-identity-" + marker + "-" + System.nanoTime();
        try (Context context = Zlink.createContext();
             RouterSocket listener = context.createRouterSocket();
             RouterSocket initiator = context.createRouterSocket();
             ExecutorService responder = Executors.newSingleThreadExecutor(task -> {
                 Thread thread = new Thread(task, "zlink-route-inbound-identity-responder");
                 thread.setDaemon(true);
                 return thread;
             })) {
            listener.setRoutingId(listenerRid);
            listener.bind(endpoint);
            initiator.setRoutingId(initiatorRid);

            connectWithProbe(initiator, listenerRid, endpoint);
            assertInboundProbe(listener, initiatorRid);
            assertReverseRequest(listener, initiator, responder, initiatorRid, marker);
        }
    }

    private static void connectWithProbe(
        RouterSocket initiator,
        RoutingId listenerRid,
        String endpoint) {
        initiator.options().setConnectRoutingId(listenerRid);
        initiator.options().probe(true);
        initiator.connect(endpoint);
    }

    private static void assertInboundProbe(RouterSocket listener, RoutingId initiatorRid) {
        try (Received received = new Received()) {
            awaitCondition(() -> {
                try {
                    return listener.recv(received, RecvFlags.DONT_WAIT);
                } catch (ZlinkRecvException ex) {
                    return false;
                }
            });
            assertEquals(initiatorRid, received.getRoutingId().orElseThrow());
            assertEquals(1, received.parts().size());
            assertEquals(0, received.parts().get(0).size());
        }
    }

    private static void assertReverseRequest(
        RouterSocket requester,
        RouterSocket responderSocket,
        ExecutorService responder,
        RoutingId targetRid,
        String marker)
        throws Exception {
        CompletableFuture<Void> server = CompletableFuture.runAsync(() -> {
            try (Received received = new Received()) {
                responderSocket.recv(received, RecvFlags.NONE);
                assertEquals(targetRid, responderSocket.getRoutingId());
                assertArrayEquals(
                    ("request:" + marker).getBytes(java.nio.charset.StandardCharsets.UTF_8),
                    received.singlePartOrThrow().toByteArray());
                received.reply()
                    .message(Message.from(("reply:" + marker).getBytes()))
                    .submit();
            }
        }, responder);

        List<Message> reply = requester.request(targetRid)
            .message(Message.from(("request:" + marker).getBytes()))
            .timeout(Duration.ofSeconds(2))
            .submit()
            .toCompletableFuture()
            .get(3, TimeUnit.SECONDS);
        try {
            assertEquals(1, reply.size());
            assertArrayEquals(
                ("reply:" + marker).getBytes(java.nio.charset.StandardCharsets.UTF_8),
                reply.get(0).toByteArray());
        } finally {
            Message.closeAll(reply);
        }
        server.get(3, TimeUnit.SECONDS);
    }

    private static void assumeNative() {
        try {
            Zlink.version();
        } catch (Throwable error) {
            throw new AssertionError("zlink native library not found: " + error.getMessage(), error);
        }
    }

    private static void awaitCondition(BooleanSupplier condition) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (condition.getAsBoolean()) {
                return;
            }
            try {
                Thread.sleep(10);
            } catch (InterruptedException ex) {
                Thread.currentThread().interrupt();
                throw new AssertionError(ex);
            }
        }
        throw new AssertionError("condition did not become true within 5s");
    }

    @FunctionalInterface
    private interface BooleanSupplier {
        boolean getAsBoolean();
    }
}
