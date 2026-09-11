/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.RepeatedTest;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;

final class SubmitResultTerminalContractTest {
    private static final long HWM_BYTES = 512L;
    private static final int PAYLOAD_BYTES = 64;
    private static final int MAX_FILL_RECORDS = 4_096;
    private static final long CLIENT_SLOT = 1L;
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final Duration WAIT_TIMEOUT =
        Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS);

    @RepeatedTest(5)
    void immediateAdmissionReturnsOkAndCompletedAdmissionBeforeReply()
        throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket server = context.createRouterSocket();
             DealerSocket client = context.createDealerSocket()) {
            connectReady(server, client, "submit-result-immediate");

            try (Message payload = Message.from("send-ok")) {
                SendSubmission send = client.send().message(payload).submit();
                assertEquals(SubmitResult.OK, send.result());
                assertTrue(send.admitted().toCompletableFuture().isDone());
                send.admitted().toCompletableFuture().join();
            }
            assertReceived(server, "send-ok");

            RequestSubmission request;
            try (Message payload = Message.from("request-ok")) {
                request = client.request().message(payload)
                    .timeout(REQUEST_TIMEOUT).submit();
            }
            assertEquals(SubmitResult.OK, request.result());
            assertTrue(request.admitted().toCompletableFuture().isDone());
            request.admitted().toCompletableFuture().join();
            assertFalse(request.reply().toCompletableFuture().isDone());

            replyOnce(server, "request-ok", "reply-ok");
            assertReply(request, "reply-ok");
        }
    }

    @RepeatedTest(5)
    void hwmBackpressureReturnsSnapshotAndAdmitsAfterWritableBeforeReply()
        throws Exception {
        TestSupport.assumeNative();

        verifyBackpressuredSend();
        verifyBackpressuredRequest();
    }

    private static void verifyBackpressuredSend() throws Exception {
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (RouterSocket server = context.createRouterSocket();
                 DealerSocket client = context.createDealerSocket();
                 Poller clientPoller = Zlink.createPoller()) {
                configureSmallHwm(server);
                configureSmallHwm(client);
                connectReady(server, client, "submit-result-send-hwm");
                clientPoller.add(client, CLIENT_SLOT,
                    PollEventFlags.POLLOUT,
                    PollEventFlags.POLLCOMPLETION);

                SendSubmission waiting = null;
                int admittedCount = 0;
                for (int sequence = 0;
                     sequence < MAX_FILL_RECORDS; sequence++) {
                    try (Message payload = Message.from(payload(sequence))) {
                        SendSubmission submission = client.send()
                            .message(payload).submit();
                        if (submission.result() == SubmitResult.BACKPRESSURED) {
                            waiting = submission;
                            break;
                        }
                        assertEquals(SubmitResult.OK, submission.result());
                        assertTrue(submission.admitted()
                            .toCompletableFuture().isDone());
                        admittedCount++;
                    }
                }

                assertTrue(admittedCount > 0);
                assertTrue(waiting != null, "send did not reach HWM");
                assertEquals(SubmitResult.BACKPRESSURED, waiting.result());
                assertFalse(waiting.admitted().toCompletableFuture().isDone());
                assertNotWritable(clientPoller);

                for (int sequence = 0; sequence < admittedCount; sequence++) {
                    assertReceived(server, payload(sequence));
                }
                SendSubmission backpressured = waiting;
                awaitPollUntil(clientPoller, () -> backpressured.admitted()
                    .toCompletableFuture().isDone());
                backpressured.admitted().toCompletableFuture().get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertEquals(SubmitResult.BACKPRESSURED,
                    backpressured.result());
                assertReceived(server, payload(admittedCount));
            }
        }
    }

    private static void verifyBackpressuredRequest() throws Exception {
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (RouterSocket server = context.createRouterSocket();
                 DealerSocket client = context.createDealerSocket();
                 Poller clientPoller = Zlink.createPoller()) {
                configureSmallHwm(server);
                configureSmallHwm(client);
                connectReady(server, client, "submit-result-request-hwm");
                clientPoller.add(client, CLIENT_SLOT,
                    PollEventFlags.POLLOUT,
                    PollEventFlags.POLLCOMPLETION);

                List<RequestSubmission> admitted = new ArrayList<>();
                RequestSubmission waiting = null;
                for (int sequence = 0;
                     sequence < MAX_FILL_RECORDS; sequence++) {
                    try (Message payload = Message.from(payload(sequence))) {
                        RequestSubmission submission = client.request()
                            .message(payload).timeout(REQUEST_TIMEOUT).submit();
                        if (submission.result() == SubmitResult.BACKPRESSURED) {
                            waiting = submission;
                            break;
                        }
                        assertEquals(SubmitResult.OK, submission.result());
                        assertTrue(submission.admitted()
                            .toCompletableFuture().isDone());
                        admitted.add(submission);
                    }
                }

                assertFalse(admitted.isEmpty());
                assertTrue(waiting != null, "request did not reach HWM");
                assertEquals(SubmitResult.BACKPRESSURED, waiting.result());
                assertFalse(waiting.admitted().toCompletableFuture().isDone());
                assertFalse(waiting.reply().toCompletableFuture().isDone());
                assertNotWritable(clientPoller);

                for (int sequence = 0; sequence < admitted.size(); sequence++) {
                    replyOnce(server, payload(sequence), "reply-" + sequence);
                }
                RequestSubmission backpressured = waiting;
                awaitPollUntil(clientPoller, () -> backpressured.admitted()
                    .toCompletableFuture().isDone());
                backpressured.admitted().toCompletableFuture().get(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
                assertEquals(SubmitResult.BACKPRESSURED,
                    backpressured.result());
                assertFalse(backpressured.reply().toCompletableFuture().isDone(),
                    "reply must not complete before the admitted request is handled");

                replyOnce(server, payload(admitted.size()), "reply-waiting");
                awaitPollUntil(clientPoller, () -> backpressured.reply()
                    .toCompletableFuture().isDone());
                for (int sequence = 0; sequence < admitted.size(); sequence++) {
                    assertReply(admitted.get(sequence), "reply-" + sequence);
                }
                assertReply(backpressured, "reply-waiting");
            }
        }
    }

    private static void connectReady(RouterSocket server,
                                     DealerSocket client,
                                     String label) {
        String endpoint = TestSupport.inprocEndpoint(label);
        server.bind(endpoint);
        client.connect(endpoint);
        try (Message probe = Message.from("ready")) {
            client.send().message(probe).submit_sync();
        }
        assertReceived(server, "ready");
    }

    private static void configureSmallHwm(
            systems.zlink.contracts.sockets.Socket socket) {
        socket.options().linger(Duration.ZERO);
        socket.options().sendHwm(HWM_BYTES);
        socket.options().recvHwm(HWM_BYTES);
    }

    private static void assertNotWritable(Poller poller) {
        PollEvents events = new PollEvents(1);
        assertEquals(0, poller.wait(events, Duration.ZERO));
    }

    private static void awaitPollUntil(Poller poller,
                                       BooleanSupplier completed) {
        PollEvents events = new PollEvents(4);
        for (int attempt = 0; attempt < 32 && !completed.getAsBoolean();
             attempt++) {
            assertTrue(poller.wait(events, WAIT_TIMEOUT) > 0,
                "completion owner did not expose progress");
            boolean matched = false;
            for (int index = 0; index < events.readyCount(); index++) {
                if (events.slot(index) == CLIENT_SLOT
                    && events.hasEvent(index,
                        PollEventFlags.POLLCOMPLETION)) {
                    matched = true;
                }
            }
            assertTrue(matched, "client completion event was not reported");
        }
        assertTrue(completed.getAsBoolean(),
            "submission did not complete after public completion progress");
    }

    private static void replyOnce(RouterSocket server,
                                  String expected,
                                  String reply) {
        try (Received received = new Received()) {
            assertTrue(server.recv(received, RecvFlags.NONE));
            assertEquals(expected,
                received.singlePartOrThrow().toUtf8String());
            try (Message payload = Message.from(reply)) {
                received.reply().message(payload).submit();
            }
        }
    }

    private static void replyOnce(RouterSocket server,
                                  byte[] expected,
                                  String reply) {
        try (Received received = new Received()) {
            assertTrue(server.recv(received, RecvFlags.NONE));
            assertArrayEquals(expected,
                received.singlePartOrThrow().toByteArray());
            try (Message payload = Message.from(reply)) {
                received.reply().message(payload).submit();
            }
        }
    }

    private static void assertReceived(RouterSocket server,
                                       String expected) {
        try (Received received = new Received()) {
            assertTrue(server.recv(received, RecvFlags.NONE));
            assertEquals(expected,
                received.singlePartOrThrow().toUtf8String());
        }
    }

    private static void assertReceived(RouterSocket server,
                                       byte[] expected) {
        try (Received received = new Received()) {
            assertTrue(server.recv(received, RecvFlags.NONE));
            assertArrayEquals(expected,
                received.singlePartOrThrow().toByteArray());
        }
    }

    private static void assertReply(RequestSubmission submission,
                                    String expected) throws Exception {
        List<Message> reply = submission.reply().toCompletableFuture().get(
            TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS);
        try {
            assertEquals(expected, reply.getFirst().toUtf8String());
        } finally {
            Message.closeAll(reply);
        }
    }

    private static byte[] payload(int sequence) {
        byte[] payload = new byte[PAYLOAD_BYTES];
        Arrays.fill(payload, (byte) 0x5a);
        ByteBuffer.wrap(payload).putInt(sequence);
        return payload;
    }
}
