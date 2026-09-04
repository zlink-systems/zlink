/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;

class DontWaitBackpressureContractTest {
    private static final long HWM_BYTES = 512L;
    private static final int PAYLOAD_BYTES = 64;
    private static final int MAX_FILL_RECORDS = 4_096;
    private static final long SENDER_SLOT = 1L;
    private static final long RECEIVER_SLOT = 2L;
    private static final Duration WAIT_TIMEOUT =
        Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS);

    @Test
    void asyncSendRetriesExactPacketAfterWritableCompletion() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (PairSocket sender = context.createPairSocket();
                 PairSocket receiver = context.createPairSocket();
                 Poller senderPoller = Zlink.createPoller();
                 Poller receiverPoller = Zlink.createPoller()) {
                configureSmallHwm(sender);
                configureSmallHwm(receiver);
                sender.options().sendTimeout(WAIT_TIMEOUT);

                String endpoint = TestSupport.inprocEndpoint(
                    "dontwait-backpressure");
                receiver.bind(endpoint);
                sender.connect(endpoint);

                receiverPoller.add(receiver, RECEIVER_SLOT,
                    PollEventFlags.POLLIN);
                try (Message probe = Message.from(payload(Integer.MIN_VALUE))) {
                    sender.send().message(probe).submit_sync();
                }
                PollEvents receiverEvents = new PollEvents(1);
                assertEquals(1, receiverPoller.wait(receiverEvents,
                    WAIT_TIMEOUT), "connection probe did not reach the peer");
                assertEquals(RECEIVER_SLOT, receiverEvents.slot(0));
                assertTrue(receiverEvents.hasEvent(0,
                    PollEventFlags.POLLIN));
                assertReceivedPayload(receiver, payload(Integer.MIN_VALUE));

                senderPoller.add(sender, SENDER_SLOT,
                    PollEventFlags.POLLOUT,
                    PollEventFlags.POLLCOMPLETION);

                int admitted = 0;
                int rejectedSequence = -1;
                Message rejectedMessage = null;
                CompletableFuture<Void> writableRetry = null;
                try {
                    for (int sequence = 0;
                         sequence < MAX_FILL_RECORDS; sequence++) {
                        Message candidate = Message.from(payload(sequence));
                        CompletableFuture<Void> completion;
                        try {
                            completion = sender.send().message(candidate)
                                .submit().toCompletableFuture();
                        } catch (RuntimeException | Error failure) {
                            candidate.close();
                            throw failure;
                        }

                        if (!completion.isDone()) {
                            rejectedSequence = sequence;
                            rejectedMessage = candidate;
                            writableRetry = completion;
                            break;
                        }

                        try {
                            completion.join();
                        } finally {
                            candidate.close();
                        }
                        admitted++;
                    }

                    assertTrue(admitted > 0,
                        "DONTWAIT fill did not admit any record");
                    assertNotNull(writableRetry,
                        "DONTWAIT fill did not reach HWM");
                    assertNotNull(rejectedMessage);
                    assertEquals(admitted, rejectedSequence);
                    assertFalse(writableRetry.isDone(),
                        "backpressured send must wait for matching WRITABLE");

                    PollEvents senderEvents = new PollEvents(1);
                    assertEquals(0, senderPoller.wait(senderEvents,
                        Duration.ZERO),
                        "sender remained writable after HWM backpressure");

                    for (int sequence = 0; sequence < admitted; sequence++) {
                        assertEquals(1, receiverPoller.wait(receiverEvents,
                            WAIT_TIMEOUT),
                            "peer did not expose an admitted record");
                        assertEquals(RECEIVER_SLOT, receiverEvents.slot(0));
                        assertTrue(receiverEvents.hasEvent(0,
                            PollEventFlags.POLLIN));
                        assertReceivedPayload(receiver, payload(sequence));
                    }

                    senderEvents = new PollEvents(1);
                    assertEquals(1, senderPoller.wait(senderEvents,
                        WAIT_TIMEOUT),
                        "peer drain did not wake the sender");
                    assertEquals(SENDER_SLOT, senderEvents.slot(0));
                    assertTrue(senderEvents.hasEvent(0,
                        PollEventFlags.POLLOUT));
                    assertTrue(senderEvents.hasEvent(0,
                        PollEventFlags.POLLCOMPLETION));

                    writableRetry.get(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS);
                    assertFalse(writableRetry.isCompletedExceptionally());

                    assertEquals(1, receiverPoller.wait(receiverEvents,
                        WAIT_TIMEOUT),
                        "retried packet did not reach the peer");
                    assertTrue(receiverEvents.hasEvent(0,
                        PollEventFlags.POLLIN));
                    assertReceivedPayload(receiver,
                        payload(rejectedSequence));

                    PollEvents noDuplicate = new PollEvents(1);
                    assertEquals(0, receiverPoller.wait(noDuplicate,
                        Duration.ZERO),
                        "the backpressured packet was delivered more than once");
                    try (Received extra = new Received()) {
                        assertFalse(receiver.recv(extra, RecvFlags.DONT_WAIT),
                            "the backpressured packet was delivered more than once");
                    }
                } finally {
                    if (rejectedMessage != null) {
                        rejectedMessage.close();
                    }
                }
            }
        }
    }

    private static void configureSmallHwm(PairSocket socket) {
        socket.options().linger(Duration.ZERO);
        socket.options().sendHwm(HWM_BYTES);
        socket.options().recvHwm(HWM_BYTES);
    }

    private static void assertReceivedPayload(PairSocket receiver,
                                              byte[] expected) {
        try (Received received = new Received()) {
            assertTrue(receiver.recv(received, RecvFlags.DONT_WAIT));
            assertArrayEquals(expected,
                received.singlePartOrThrow().toByteArray());
        }
    }

    private static byte[] payload(int sequence) {
        byte[] payload = new byte[PAYLOAD_BYTES];
        Arrays.fill(payload, (byte) 0x5a);
        ByteBuffer.wrap(payload).putInt(sequence);
        return payload;
    }
}
