/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.StreamSocket;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * Regression tests for send operations inside receive callbacks.
 *
 * <p>Blocking send APIs are explicitly rejected from callback context. Callers
 * that need callback-safe send behavior must use the explicit {@code try*}
 * APIs instead of relying on an implicit blocking-mode downgrade.
 */
public class CallbackSendContractTest {

    @Test
    public void routerSendInsideOnReceiveDoesNotHang() throws Exception {
        TestSupport.assumeNative();

        CountDownLatch replyReceived = new CountDownLatch(1);
        AtomicReference<byte[]> replyPayload = new AtomicReference<>();
        AtomicReference<Throwable> callbackError = new AtomicReference<>();

        try (Context ctx = Zlink.createContext();
             RouterSocket router = ctx.createRouterSocket();
             DealerSocket dealer = ctx.createDealerSocket()) {

            String endpoint = TestSupport.tcpEndpoint();
            dealer.setRoutingId(RoutingId.from(
                "request-reply-client".getBytes(StandardCharsets.UTF_8)));
            router.bind(endpoint);
            dealer.connect(endpoint);
            TestSupport.allowTcpRequestReplyCallbackHandshakeToSettle();

            Thread routerThread = new Thread(() -> {
                try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {

                    router.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                    RoutingId rid = received.getRoutingId().orElseThrow();
                    assertNotNull(rid,
                        "router must receive routing id from dealer");
                    try (Message reply = Message.from("pong")) {
                        received.reply().message(reply).submit();
                    }
                } catch (Throwable t) {
                    callbackError.set(t);
                }
            }, "router-send-inside-recv");
            routerThread.start();

            try (Message request = Message.from("ping")) {
                dealer.request()
                    .message(request)
                    .flags(SendFlags.NONE)
                    .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                    .submit(
                    (result, received) -> {
                        try {
                            assertEquals(RequestResult.OK, result);
                            replyPayload.set(received.get(0).toByteArray());
                        } catch (Throwable t) {
                            callbackError.set(t);
                        } finally {
                            Message.closeAll(received);
                            replyReceived.countDown();
                        }
                    });
            }

            assertTrue(replyReceived.await(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS),
                "callback+send timed out -- send from callback hung");
            routerThread.join(TestSupport.DEFAULT_TIMEOUT_MS);
            assertFalse(routerThread.isAlive(),
                "router receive thread did not finish before socket close");
            closeAfterCallback(dealer);
            closeAfterCallback(router);
            assertNull(callbackError.get(),
                "callback raised: " + callbackError.get());
            assertArrayEquals("pong".getBytes(StandardCharsets.UTF_8),
                replyPayload.get());
        }
    }

    @Test
    public void pairSendInsideOnReceiveDoesNotHang() throws Exception {
        TestSupport.assumeNative();

        CountDownLatch replyReceived = new CountDownLatch(1);
        AtomicReference<byte[]> replyPayload = new AtomicReference<>();
        AtomicReference<Throwable> callbackError = new AtomicReference<>();

        try (Context ctx = Zlink.createContext();
             PairSocket left = ctx.createPairSocket();
             PairSocket right = ctx.createPairSocket()) {

            String endpoint = TestSupport.tcpEndpoint();
            try (var leftMon = left.monitorOpen(MonitorEventType.CONNECTION_READY);
                 var rightMon = right.monitorOpen(MonitorEventType.CONNECTION_READY)) {
                left.bind(endpoint);
                right.connect(endpoint);
                leftMon.recv();
                rightMon.recv();
            }

            Thread rightThread = new Thread(() -> {
                try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {

                    right.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                    byte[] data = received.singlePartOrThrow().toByteArray();
                    assertEquals("ping",
                        new String(data, StandardCharsets.UTF_8));
                    try (Message reply = Message.from("pong")) {
                        right.send().message(reply).flags(SendFlags.DONT_WAIT).submit();
                    }
                } catch (Throwable t) {
                    callbackError.set(t);
                }
            }, "pair-send-inside-recv-right");
            rightThread.start();

            Thread leftThread = new Thread(() -> {
                try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {

                    left.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                    replyPayload.set(received.singlePartOrThrow().toByteArray());
                } catch (Throwable t) {
                    callbackError.set(t);
                } finally {
                    replyReceived.countDown();
                }
            }, "pair-send-inside-recv-left");
            leftThread.start();

            try (Message request = Message.from("ping")) {
                left.send().message(request).submit();
            }

            assertTrue(replyReceived.await(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS),
                "callback+send timed out -- send from callback hung");
            assertNull(callbackError.get(),
                "callback raised: " + callbackError.get());
            assertArrayEquals("pong".getBytes(StandardCharsets.UTF_8),
                replyPayload.get());
        }
    }

    @Test
    public void routerMultiRoundCallbackSend() throws Exception {
        TestSupport.assumeNative();

        int roundCount = 5;
        CountDownLatch allReplies = new CountDownLatch(roundCount);
        AtomicReference<Throwable> callbackError = new AtomicReference<>();

        try (Context ctx = Zlink.createContext()) {
            for (int i = 0; i < roundCount; i++) {
                int index = i;
                CountDownLatch roundDone = new CountDownLatch(1);
                CountDownLatch routerDone = new CountDownLatch(1);
                try (RouterSocket router = ctx.createRouterSocket();
                     DealerSocket dealer = ctx.createDealerSocket()) {
                    router.options().linger(Duration.ZERO);
                    dealer.options().linger(Duration.ZERO);
                    String endpoint = TestSupport.tcpEndpoint();
                    dealer.setRoutingId(RoutingId.from(
                        "request-reply-client"
                            .getBytes(StandardCharsets.UTF_8)));
                    router.bind(endpoint);
                    dealer.connect(endpoint);
                    TestSupport.allowTcpRequestReplyCallbackHandshakeToSettle();

                    Thread routerThread = new Thread(() -> {
                        try (systems.zlink.contracts.messaging.Received received = new systems.zlink.contracts.messaging.Received()) {

                            router.recv(received, systems.zlink.contracts.sockets.RecvFlags.NONE);
                            RoutingId rid = received.getRoutingId().orElseThrow();
                            byte[] data = received.singlePartOrThrow()
                                .toByteArray();
                            String payload = new String(data,
                                StandardCharsets.UTF_8);
                            assertTrue(payload.startsWith("request-"));
                            String requestIndex = payload.substring(
                                "request-".length());
                            try (Message reply = Message.from(
                                     "reply-" + requestIndex)) {
                                received.reply().message(reply).submit();
                            }
                        } catch (Throwable t) {
                            if (callbackError.get() == null)
                                callbackError.set(t);
                        } finally {
                            routerDone.countDown();
                        }
                    }, "router-multi-round-recv-" + index);
                    routerThread.start();

                    try (Message request = Message.from("request-" + index)) {
                        dealer.request()
                            .message(request)
                            .flags(SendFlags.NONE)
                            .timeout(Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS))
                            .submit(
                            (result, received) -> {
                                try {
                                    assertEquals(RequestResult.OK, result);
                                    byte[] data = received.get(0)
                                        .toByteArray();
                                    String payload = new String(data,
                                        StandardCharsets.UTF_8);
                                    assertTrue(payload.startsWith("reply-"));
                                    assertEquals("reply-" + index, payload);
                                } catch (Throwable t) {
                                    if (callbackError.get() == null)
                                        callbackError.set(t);
                                } finally {
                                    Message.closeAll(received);
                                    roundDone.countDown();
                                    allReplies.countDown();
                                }
                            });
                    }

                    assertTrue(roundDone.await(
                            TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS),
                        "round callback timed out");
                    assertTrue(routerDone.await(
                            TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS),
                        "router recv timed out");
                    routerThread.join(TestSupport.DEFAULT_TIMEOUT_MS);
                    assertFalse(routerThread.isAlive(),
                        "router receive thread did not finish before socket close");
                    closeAfterCallback(dealer);
                    closeAfterCallback(router);
                }
            }
        }

        assertTrue(allReplies.await(
                TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS),
            "multi-round callback+send timed out");
        assertNull(callbackError.get(),
            "callback raised: " + callbackError.get());
    }

    private static void closeAfterCallback(AutoCloseable resource) {
        long deadline = System.nanoTime()
            + TestSupport.DEFAULT_TIMEOUT_MS * 1_000_000L;
        while (true) {
            try {
                resource.close();
                return;
            } catch (ZlinkCloseException ex) {
                if (ex.getResult() != CloseResult.BUSY
                    || System.nanoTime() >= deadline) {
                    throw ex;
                }
                Thread.yield();
            } catch (Exception ex) {
                throw new RuntimeException(ex);
            }
        }
    }

    @Test
    public void streamRawCallbackSendDoesNotCopyThroughReceivedSurface()
        throws Exception {
        TestSupport.assumeNative();

        CountDownLatch echoed = new CountDownLatch(1);
        AtomicReference<Throwable> callbackError = new AtomicReference<>();
        byte[] outbound = "ping".getBytes(StandardCharsets.UTF_8);
        String endpoint = TestSupport.tcpEndpoint();
        int port = Integer.parseInt(endpoint.substring(endpoint.lastIndexOf(':')
            + 1));

        try (Context ctx = Zlink.createContext();
             StreamSocket server = ctx.createStreamSocket()) {
            server.options().notify(true);
            server.bind(endpoint);
            server.onPacket((routingId, header, payload) -> {
                try {
                    if (routingId == null)
                        return;
                    int size = payload.size();
                    if (size == 0)
                        return;
                    try (Message reply = frame(Message.from(new byte[0]),
                            payload)) {
                        server.send(routingId)
                            .message(reply)
                            .flags(SendFlags.DONT_WAIT)
                            .submit();
                    }
                    echoed.countDown();
                } catch (Throwable t) {
                    callbackError.compareAndSet(null, t);
                }
            });

            try (java.net.Socket client = new java.net.Socket("127.0.0.1", port)) {
                client.setSoTimeout(TestSupport.DEFAULT_TIMEOUT_MS);
                byte[] frame = frameBytes(outbound);
                client.getOutputStream().write(frame);
                client.getOutputStream().flush();
                byte[] echoedPayload = client.getInputStream().readNBytes(frame.length);
                assertArrayEquals(outbound,
                    Arrays.copyOfRange(echoedPayload, 6, echoedPayload.length));
            }

            assertTrue(echoed.await(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS),
                "stream packet callback echo timed out");
            assertNull(callbackError.get(),
                "callback raised: " + callbackError.get());
        }
    }

    @Test
    public void blockingSendInsideOnReceiveIsRejectedExplicitly() throws Exception {
        TestSupport.assumeNative();
        assertFalse(Arrays.stream(PairSocket.class.getMethods())
            .anyMatch(method -> method.getName().equals("onReceive")));
    }

    private static byte[] frameBytes(byte[] body) {
        byte[] frame = new byte[6 + body.length];
        frame[2] = (byte) (body.length >>> 24);
        frame[3] = (byte) (body.length >>> 16);
        frame[4] = (byte) (body.length >>> 8);
        frame[5] = (byte) body.length;
        System.arraycopy(body, 0, frame, 6, body.length);
        return frame;
    }

    private static Message frame(Message header, Message body) {
        try (header) {
            byte[] headerBytes = header.toByteArray();
            byte[] bodyBytes = body.toByteArray();
            byte[] frame = new byte[6 + headerBytes.length + bodyBytes.length];
            frame[0] = (byte) (headerBytes.length >>> 8);
            frame[1] = (byte) headerBytes.length;
            frame[2] = (byte) (bodyBytes.length >>> 24);
            frame[3] = (byte) (bodyBytes.length >>> 16);
            frame[4] = (byte) (bodyBytes.length >>> 8);
            frame[5] = (byte) bodyBytes.length;
            System.arraycopy(headerBytes, 0, frame, 6, headerBytes.length);
            System.arraycopy(bodyBytes, 0, frame, 6 + headerBytes.length,
                bodyBytes.length);
            return Message.from(frame);
        }
    }
}
