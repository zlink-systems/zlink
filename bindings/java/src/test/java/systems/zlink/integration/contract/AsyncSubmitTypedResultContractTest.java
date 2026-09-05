/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;
import org.junit.jupiter.api.DynamicTest;
import org.junit.jupiter.api.TestFactory;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SubmitResult;

class AsyncSubmitTypedResultContractTest {
    private static final long HWM_BYTES = 512L;
    private static final int PAYLOAD_BYTES = 64;
    private static final int MAX_FILL_RECORDS = 4_096;
    private static final Duration WAIT_TIMEOUT =
        Duration.ofMillis(TestSupport.DEFAULT_TIMEOUT_MS);

    @TestFactory
    Stream<DynamicTest> exactRouteLossIsNotConnected() {
        return transports("exact-route-loss", this::verifyExactRouteLoss);
    }

    @TestFactory
    Stream<DynamicTest> peerTypeAdmissionRefusalIsNotAdmitted() {
        return transports("admission-refusal",
            this::verifyAdmissionRefusal);
    }

    @TestFactory
    Stream<DynamicTest> capacityWaitCompletesAfterWritable() {
        return transports("capacity-writable", this::verifyCapacityWait);
    }

    @TestFactory
    Stream<DynamicTest> removedWaitTargetIsNotFound() {
        return transports("wait-target-removal",
            this::verifyWaitTargetRemoval);
    }

    private void verifyExactRouteLoss(Transport transport) {
        TestSupport.assumeNative();

        RoutingId dealerId = RoutingId.from("typed-route-peer");
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket()) {
            connect(transport, router, dealer, dealerId, "exact-route");
            RoutingId absent = RoutingId.from("typed-route-absent");
            try (Message payload = Message.from("missing-route")) {
                ZlinkSubmitException failure = assertThrows(
                    ZlinkSubmitException.class, () -> router.send(absent)
                        .message(payload).submit());
                assertEquals(SubmitResult.NOT_CONNECTED,
                    failure.getResult());
                assertEquals("missing-route", payload.toUtf8String());
            }
        }
    }

    private void verifyAdmissionRefusal(Transport transport) {
        TestSupport.assumeNative();

        RoutingId dealerId = RoutingId.from("typed-admission-peer");
        try (Context context = Zlink.createContext();
             RouterSocket router = context.createRouterSocket();
             DealerSocket dealer = context.createDealerSocket()) {
            connect(transport, router, dealer, dealerId, "admission");
            try (Message payload = Message.from("request-to-dealer")) {
                ZlinkSubmitException failure = assertThrows(
                    ZlinkSubmitException.class, () -> router
                        .request(dealerId).message(payload)
                        .timeout(WAIT_TIMEOUT).submit());
                assertEquals(SubmitResult.NOT_ADMITTED,
                    failure.getResult());
                assertEquals("request-to-dealer", payload.toUtf8String());
            }
        }
    }

    private void verifyCapacityWait(Transport transport) throws Exception {
        TestSupport.assumeNative();

        RoutingId dealerId = RoutingId.from("typed-capacity-peer");
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (RouterSocket router = context.createRouterSocket();
                 DealerSocket dealer = context.createDealerSocket()) {
                configureSmallHwm(router);
                configureSmallHwm(dealer);
                connect(transport, router, dealer, dealerId, "capacity");

                PendingSend pending = fillUntilBackpressured(router,
                    dealerId);
                assertFalse(pending.completion().isDone(),
                    "BACKPRESSURED submit must wait for WRITABLE");

                for (int sequence = 0; sequence < pending.admitted();
                     sequence++) {
                    assertReceivedPayload(dealer, payload(sequence));
                }
                pending.completion().get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS);
                assertReceivedPayload(dealer,
                    payload(pending.rejectedSequence()));
            }
        }
    }

    private void verifyWaitTargetRemoval(Transport transport)
        throws Exception {
        TestSupport.assumeNative();

        RoutingId dealerId = RoutingId.from("typed-terminal-peer");
        try (Context context = Zlink.createContext()) {
            context.options().autoHwmEnabled(false);
            try (RouterSocket router = context.createRouterSocket();
                 DealerSocket dealer = context.createDealerSocket()) {
                configureSmallHwm(router);
                configureSmallHwm(dealer);
                connect(transport, router, dealer, dealerId, "terminal");

                PendingSend pending = fillUntilBackpressured(router,
                    dealerId);
                router.disconnectRid(dealerId);

                ExecutionException terminal = assertThrows(
                    ExecutionException.class, () -> pending.completion().get(
                        TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS));
                ZlinkSubmitException failure = (ZlinkSubmitException)
                    terminal.getCause();
                assertEquals(SubmitResult.NOT_FOUND, failure.getResult());
            }
        }
    }

    private static void connect(Transport transport, RouterSocket router,
                                DealerSocket dealer, RoutingId dealerId,
                                String purpose) {
        router.options().mandatory(true);
        dealer.setRoutingId(dealerId);
        String endpoint = transport.endpoint(purpose);
        router.bind(endpoint);
        dealer.connect(endpoint);
        try (Message probe = Message.from("ready")) {
            dealer.send().message(probe).submit_sync();
        }
        try (Received received = new Received()) {
            assertTrue(router.recv(received, RecvFlags.NONE));
        }
        try (Message probe = Message.from("ready-back")) {
            router.send(dealerId).message(probe).submit_sync();
        }
        try (Received received = new Received()) {
            assertTrue(dealer.recv(received, RecvFlags.NONE));
        }
    }

    private static PendingSend fillUntilBackpressured(RouterSocket router,
                                                       RoutingId target) {
        for (int sequence = 0; sequence < MAX_FILL_RECORDS; sequence++) {
            try (Message candidate = Message.from(payload(sequence))) {
                CompletableFuture<Void> completion = router.send(target)
                    .message(candidate).submit().toCompletableFuture();
                if (!completion.isDone()) {
                    return new PendingSend(completion, sequence, sequence);
                }
                completion.join();
            }
        }
        throw new AssertionError("DONTWAIT fill did not reach HWM");
    }

    private static void configureSmallHwm(
            systems.zlink.contracts.sockets.Socket socket) {
        socket.options().linger(Duration.ZERO);
        socket.options().sendHwm(HWM_BYTES);
        socket.options().recvHwm(HWM_BYTES);
    }

    private static void assertReceivedPayload(DealerSocket dealer,
                                              byte[] expected)
        throws InterruptedException {
        try (Received received = new Received()) {
            long deadline = System.nanoTime()
                + TimeUnit.MILLISECONDS.toNanos(
                    TestSupport.DEFAULT_TIMEOUT_MS);
            while (!dealer.recv(received, RecvFlags.DONT_WAIT)) {
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError(
                        "timed out waiting for admitted payload");
                }
                Thread.sleep(1L);
            }
            assertTrue(received.replyToken().isEmpty());
            assertEquals(1, received.parts().size());
            org.junit.jupiter.api.Assertions.assertArrayEquals(expected,
                received.singlePartOrThrow().toByteArray());
        }
    }

    private static byte[] payload(int sequence) {
        byte[] payload = new byte[PAYLOAD_BYTES];
        Arrays.fill(payload, (byte) 0x5a);
        payload[0] = (byte) (sequence >>> 24);
        payload[1] = (byte) (sequence >>> 16);
        payload[2] = (byte) (sequence >>> 8);
        payload[3] = (byte) sequence;
        return payload;
    }

    private static Stream<DynamicTest> transports(String caseName,
                                                   TransportCase test) {
        return Stream.of(Transport.values()).map(transport ->
            DynamicTest.dynamicTest(caseName + "[" + transport + "]",
                () -> test.run(transport)));
    }

    private enum Transport {
        INPROC {
            @Override String endpoint(String purpose) {
                return TestSupport.inprocEndpoint("typed-submit-" + purpose);
            }
        },
        TCP {
            @Override String endpoint(String purpose) {
                return TestSupport.tcpEndpoint();
            }
        };

        abstract String endpoint(String purpose);
    }

    @FunctionalInterface
    private interface TransportCase {
        void run(Transport transport) throws Exception;
    }

    private record PendingSend(CompletableFuture<Void> completion,
                               int admitted, int rejectedSequence) {
    }
}
