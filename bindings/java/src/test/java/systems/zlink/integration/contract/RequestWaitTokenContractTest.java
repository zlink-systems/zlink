/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.runtime.nativeapi.NativeErrno;

final class RequestWaitTokenContractTest {
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final int REQUEST_COUNT = 64;

    @RepeatedTest(5)
    void hwmBackpressuredRequestsResumeAndReceiveReplies() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            context.options().autoHwmEnabled(false);
            configureSmallHwm(server);
            configureSmallHwm(client);
            String endpoint = TestSupport.inprocEndpoint("request-token-hwm");
            server.bind(endpoint);
            client.connect(endpoint);

            List<CompletableFuture<List<Message>>> replies = new ArrayList<>();
            for (int sequence = 0; sequence < REQUEST_COUNT; sequence++) {
                try (Message request = Message.from(requestPayload(sequence))) {
                    replies.add(client.request().message(request)
                        .timeout(REQUEST_TIMEOUT).submit().toCompletableFuture());
                }
            }

            for (int sequence = 0; sequence < REQUEST_COUNT; sequence++) {
                try (Received request = new Received()) {
                    assertTrue(server.recv(request, RecvFlags.NONE));
                    assertArrayEquals(requestPayload(sequence),
                        request.singlePartOrThrow().toByteArray());
                    try (Message reply = Message.from("reply-" + sequence)) {
                        request.reply().message(reply).submit();
                    }
                }
                List<Message> reply = replies.get(sequence).get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                try {
                    assertEquals("reply-" + sequence,
                        reply.getFirst().toUtf8String());
                } finally {
                    Message.closeAll(reply);
                }
            }
        }
    }

    @Test
    void connectBeforeBindRequestWaitsForWritableAdmission() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            String endpoint = TestSupport.inprocEndpoint("request-before-bind");
            client.connect(endpoint);
            CompletableFuture<List<Message>> future;
            try (Message request = Message.from("before-bind")) {
                future = client.request().message(request)
                    .timeout(REQUEST_TIMEOUT).submit().toCompletableFuture();
            }
            server.bind(endpoint);
            replyOnce(server, "before-bind-reply");
            assertReply(future, "before-bind-reply");
        }
    }

    @Test
    void closeTerminatesRequestWaitingForWritable() {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            context.options().autoHwmEnabled(false);
            configureSmallHwm(server);
            configureSmallHwm(client);
            String endpoint = TestSupport.inprocEndpoint("request-close-token");
            server.bind(endpoint);
            client.connect(endpoint);
            CompletableFuture<List<Message>> future = null;
            for (int sequence = 0; sequence < REQUEST_COUNT; sequence++) {
                try (Message request = Message.from(requestPayload(sequence))) {
                    future = client.request().message(request)
                        .timeout(REQUEST_TIMEOUT).submit().toCompletableFuture();
                }
            }
            client.close();
            CompletableFuture<List<Message>> last = future;
            ExecutionException failure = assertThrows(ExecutionException.class,
                () -> last.get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS));
            ZlinkRequestException requestFailure = assertInstanceOf(
                ZlinkRequestException.class, failure.getCause());
            assertEquals(RequestResult.TERMINATED, requestFailure.getResult());
            assertEquals(NativeErrno.ESHUTDOWN,
                requestFailure.getNativeErrno());
        }
    }

    @Test
    void sendAndRequestTokensProgressTogether() throws Exception {
        TestSupport.assumeNative();
        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            String endpoint = TestSupport.inprocEndpoint("mixed-wait-tokens");
            client.connect(endpoint);
            CompletableFuture<Void> send;
            CompletableFuture<List<Message>> request;
            try (Message payload = Message.from("send-token")) {
                send = client.send().message(payload).submit()
                    .toCompletableFuture();
            }
            try (Message payload = Message.from("request-token")) {
                request = client.request().message(payload)
                    .timeout(REQUEST_TIMEOUT).submit().toCompletableFuture();
            }

            server.bind(endpoint);
            boolean replied = false;
            for (int index = 0; index < 2; index++) {
                try (Received received = new Received()) {
                    assertTrue(server.recv(received, RecvFlags.NONE));
                    if (received.replyToken().isPresent()) {
                        assertEquals("request-token",
                            received.singlePartOrThrow().toUtf8String());
                        try (Message reply = Message.from("mixed-reply")) {
                            received.reply().message(reply).submit();
                        }
                        replied = true;
                    } else {
                        assertEquals("send-token",
                            received.singlePartOrThrow().toUtf8String());
                    }
                }
            }
            assertTrue(replied);
            send.get(TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
            assertReply(request, "mixed-reply");
        }
    }

    private static void replyOnce(RouterSocket server, String value) {
        try (Received received = new Received()) {
            assertTrue(server.recv(received, RecvFlags.NONE));
            try (Message reply = Message.from(value)) {
                received.reply().message(reply).submit();
            }
        }
    }

    private static void assertReply(
            CompletableFuture<List<Message>> future, String expected)
            throws Exception {
        List<Message> reply = future.get(TestSupport.DEFAULT_TIMEOUT_MS,
            TimeUnit.MILLISECONDS);
        try {
            assertEquals(expected, reply.getFirst().toUtf8String());
        } finally {
            Message.closeAll(reply);
        }
    }

    private static void configureSmallHwm(
            systems.zlink.contracts.sockets.Socket socket) {
        socket.options().linger(Duration.ZERO);
        socket.options().sendHwm(512L);
        socket.options().recvHwm(512L);
    }

    private static byte[] requestPayload(int sequence) {
        byte[] payload = new byte[64];
        ByteBuffer.wrap(payload).putInt(sequence);
        return payload;
    }
}
