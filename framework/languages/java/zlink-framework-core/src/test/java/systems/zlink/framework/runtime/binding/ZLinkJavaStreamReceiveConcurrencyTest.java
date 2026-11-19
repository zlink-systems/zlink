package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.StreamSocket;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkJavaStreamReceiveConcurrencyTest {
    @Test
    void receiveDoesNotWaitForSynchronousBoundSessionSubmitOnTheStateLane() throws Exception {
        CountDownLatch sendEntered = new CountDownLatch(1);
        CountDownLatch releaseSend = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var stream =
                        new ZLinkJavaStreamSocket(
                                context.createStreamSocket(),
                                null,
                                (rid, parts, flags) -> {
                                    sendEntered.countDown();
                                    await(releaseSend);
                                    return true;
                                });
                Message frame = Message.from("bound-session-frame")) {
            stream.bind("tcp://127.0.0.1:0");
            URI endpoint = URI.create(stream.lastEndpoint());
            try (Socket client = new Socket(endpoint.getHost(), endpoint.getPort())) {
                byte[] payload = "ready-before-send".getBytes(StandardCharsets.UTF_8);
                client.getOutputStream()
                        .write(
                                ByteBuffer.allocate(6 + payload.length)
                                        .putShort((short) 0)
                                        .putInt(payload.length)
                                        .put(payload)
                                        .array());
                client.getOutputStream().flush();
                assertTrue(stream.waitForReadable(Duration.ofSeconds(5)));
                var send =
                        CompletableFuture.supplyAsync(
                                () ->
                                        stream.sendBoundSessionPush(
                                                RoutingId.from("test-peer"),
                                                List.of(frame),
                                                SendFlags.NONE));
                assertTrue(sendEntered.await(5, TimeUnit.SECONDS));
                var receive = CompletableFuture.supplyAsync(stream::recv);
                try {
                    var received = receive.get(1, TimeUnit.SECONDS);
                    assertNotNull(received);
                    assertArrayEquals(payload, received.body().data());
                } finally {
                    releaseSend.countDown();
                    assertTrue(send.get(5, TimeUnit.SECONDS));
                    // Also release packet ownership if the pre-fix lane delayed receive.
                    if (!receive.isCompletedExceptionally()) {
                        var received = receive.get(5, TimeUnit.SECONDS);
                        if (received != null) received.close();
                    }
                }
            }
        } finally {
            releaseSend.countDown();
        }
    }

    @Test
    void rejectedCloseDuringReceiveCanBeCalledAgain() throws Exception {
        CountDownLatch receiveEntered = new CountDownLatch(1);
        CountDownLatch releaseReceive = new CountDownLatch(1);
        AtomicInteger closeCalls = new AtomicInteger();
        try (var context = Zlink.createContext()) {
            StreamSocket nativeSocket = context.createStreamSocket();
            StreamSocket socket =
                    (StreamSocket)
                            Proxy.newProxyInstance(
                                    StreamSocket.class.getClassLoader(),
                                    new Class<?>[] {StreamSocket.class},
                                    (proxy, method, args) -> {
                                        if (method.getName().equals("recvPacket")) {
                                            receiveEntered.countDown();
                                            await(releaseReceive);
                                        }
                                        if (method.getName().equals("close")) {
                                            closeCalls.incrementAndGet();
                                            if (releaseReceive.getCount() != 0) {
                                                // Model binding's fail-fast rejection while its
                                                // receive reservation is held.
                                                throw new ZlinkCloseException(CloseResult.BUSY);
                                            }
                                        }
                                        try {
                                            return method.invoke(nativeSocket, args);
                                        } catch (InvocationTargetException failure) {
                                            throw failure.getCause();
                                        }
                                    });
            try (var stream = new ZLinkJavaStreamSocket(socket, null)) {
                var receive = CompletableFuture.supplyAsync(stream::recv);
                try {
                    assertTrue(receiveEntered.await(5, TimeUnit.SECONDS));
                    var close =
                            CompletableFuture.runAsync(
                                    () -> {
                                        var failure =
                                                assertThrows(
                                                        ZlinkCloseException.class, stream::close);
                                        assertEquals(CloseResult.BUSY, failure.getResult());
                                    });
                    close.get(1, TimeUnit.SECONDS);
                } finally {
                    releaseReceive.countDown();
                    assertNull(receive.get(5, TimeUnit.SECONDS));
                }
                assertNull(stream.recv());
                stream.close();
                stream.close();
                assertEquals(2, closeCalls.get(), "one rejected and one accepted native close");
            } finally {
                releaseReceive.countDown();
                nativeSocket.close();
            }
        }
    }

    @Test
    void nativeReceiveAndClosePreservePacketOrReturnTheCloseResult() throws Exception {
        try (var context = Zlink.createContext()) {
            for (int round = 0; round < 16; round++) {
                try (var stream = new ZLinkJavaStreamSocket(context.createStreamSocket(), null)) {
                    stream.bind("tcp://127.0.0.1:0");
                    URI endpoint = URI.create(stream.lastEndpoint());
                    try (Socket client = new Socket(endpoint.getHost(), endpoint.getPort())) {
                        client.getOutputStream().write(new byte[] {0, 0, 0, 0, 0, 1, 42});
                        client.getOutputStream().flush();
                        assertTrue(stream.waitForReadable(Duration.ofSeconds(5)));
                        CountDownLatch start = new CountDownLatch(1);
                        var receive =
                                CompletableFuture.supplyAsync(
                                        () -> {
                                            await(start);
                                            try (var received = stream.recv()) {
                                                assertNotNull(received);
                                                assertTrue(received.routingId().isPresent());
                                                assertArrayEquals(
                                                        new byte[] {42}, received.body().data());
                                                return RecvResult.OK;
                                            } catch (ZlinkRecvException failure) {
                                                return failure.getResult();
                                            }
                                        });
                        var close =
                                CompletableFuture.supplyAsync(
                                        () -> {
                                            await(start);
                                            try {
                                                stream.close();
                                                return CloseResult.OK;
                                            } catch (ZlinkCloseException failure) {
                                                assertEquals(CloseResult.BUSY, failure.getResult());
                                                return failure.getResult();
                                            }
                                        });
                        start.countDown();
                        CompletableFuture.allOf(receive, close).get(5, TimeUnit.SECONDS);
                        RecvResult outcome = receive.get();
                        if (outcome != RecvResult.OK) {
                            assertEquals(
                                    CloseResult.OK,
                                    close.get(),
                                    "a rejected close must preserve the ready packet");
                            // Core Errors §4.3 and §5: teardown -> INVALID_HANDLE;
                            // accepted close racing receive admission -> INVALID_STATE.
                            assertTrue(
                                    outcome == RecvResult.INVALID_HANDLE
                                            || outcome == RecvResult.INVALID_STATE,
                                    () -> "unexpected receive result: " + outcome);
                        }
                        // The caller explicitly closes after both competing API calls have ended.
                        stream.close();
                    }
                }
            }
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS), "test operation was not released");
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }
}
