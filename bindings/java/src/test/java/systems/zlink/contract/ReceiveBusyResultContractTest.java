/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.SendSubmitOperation;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;

class ReceiveBusyResultContractTest {
    private static final int CONCURRENT_RECEIVERS = 8;
    private static final int PARTS_PER_RECORD = 512;
    private static final int REPRO_ROUNDS = 24;

    @Test
    void concurrentPairReceivePreservesCoreBusyResult() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket()) {
            String endpoint = TestSupport.inprocEndpoint(
                "binding-audit-java-b8-pair-busy");
            receiver.bind(endpoint);
            sender.connect(endpoint);
            assertBusyIsPreserved(sender::send, receiver::recv);
        }
    }

    @Test
    void concurrentRouterReceivePreservesCoreBusyResult() throws Exception {
        TestSupport.assumeNative();

        try (Context context = Zlink.createContext();
             RouterSocket receiver = context.createRouterSocket();
             DealerSocket sender = context.createDealerSocket()) {
            String endpoint = TestSupport.inprocEndpoint(
                "binding-audit-java-b8-router-busy");
            receiver.bind(endpoint);
            sender.connect(endpoint);
            assertBusyIsPreserved(sender::send, receiver::recv);
        }
    }

    private static void assertBusyIsPreserved(SendFactory sender,
                                               ReceiveOperation receiver)
        throws Exception {
        sendMultipart(sender, 1, "ready");
        try (Received received = new Received()) {
            assertTrue(receiver.recv(received, RecvFlags.NONE));
        }

        try (ExecutorService workers =
                 Executors.newFixedThreadPool(CONCURRENT_RECEIVERS)) {
            boolean sawBusyException = false;
            boolean sawRefusalAsNoData = false;
            for (int round = 0; round < REPRO_ROUNDS && !sawBusyException
                && !sawRefusalAsNoData; round++) {
                for (int record = 0; record < CONCURRENT_RECEIVERS; record++) {
                    sendMultipart(sender, PARTS_PER_RECORD,
                        round + "-" + record);
                }

                CyclicBarrier start =
                    new CyclicBarrier(CONCURRENT_RECEIVERS + 1);
                @SuppressWarnings("unchecked")
                Future<ReceiveAttempt>[] futures = new Future[
                    CONCURRENT_RECEIVERS];
                for (int index = 0; index < CONCURRENT_RECEIVERS; index++) {
                    futures[index] = workers.submit(() -> {
                        try (Received received = new Received()) {
                            start.await();
                            try {
                                return ReceiveAttempt.fromResult(
                                    receiver.recv(received,
                                        RecvFlags.DONT_WAIT));
                            } catch (ZlinkRecvException error) {
                                return ReceiveAttempt.fromError(
                                    error.getResult());
                            }
                        }
                    });
                }

                start.await();
                int successfulReads = 0;
                int hiddenRefusals = 0;
                for (Future<ReceiveAttempt> future : futures) {
                    ReceiveAttempt attempt = future.get(10,
                        TimeUnit.SECONDS);
                    sawBusyException |= attempt.error() == RecvResult.BUSY;
                    successfulReads += Boolean.TRUE.equals(attempt.received())
                        ? 1 : 0;
                    hiddenRefusals += Boolean.FALSE.equals(attempt.received())
                        ? 1 : 0;
                }
                sawRefusalAsNoData = hiddenRefusals > 0
                    && successfulReads + hiddenRefusals
                        == CONCURRENT_RECEIVERS
                    && successfulReads < CONCURRENT_RECEIVERS;

                while (true) {
                    try (Received remaining = new Received()) {
                        if (!receiver.recv(remaining, RecvFlags.DONT_WAIT)) {
                            break;
                        }
                    }
                }
            }

            assertTrue(sawBusyException,
                sawRefusalAsNoData
                    ? "Core returned BUSY during multipart receive, but recv returned false."
                    : "Core BUSY was not observed across " + REPRO_ROUNDS
                        + " concurrent rounds with " + PARTS_PER_RECORD
                        + " parts per record.");

            try (Received empty = new Received()) {
                assertFalse(receiver.recv(empty, RecvFlags.DONT_WAIT));
            }
        }
    }

    private static void sendMultipart(SendFactory sender, int partCount,
                                      String payload) {
        SendSubmitOperation operation =
            sender.send().message(Message.from(payload));
        for (int index = 1; index < partCount; index++) {
            operation = operation.message(Message.from(payload));
        }
        operation.submit_sync();
    }

    @FunctionalInterface
    private interface SendFactory {
        SendOperation send();
    }

    @FunctionalInterface
    private interface ReceiveOperation {
        boolean recv(Received target, RecvFlags flags);
    }

    private record ReceiveAttempt(Boolean received, RecvResult error) {
        static ReceiveAttempt fromResult(boolean received) {
            return new ReceiveAttempt(received, null);
        }

        static ReceiveAttempt fromError(RecvResult error) {
            return new ReceiveAttempt(null, error);
        }
    }
}
