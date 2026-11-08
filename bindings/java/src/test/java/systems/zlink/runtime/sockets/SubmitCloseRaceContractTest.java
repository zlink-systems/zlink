/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.PollEventFlags;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.DealerSocket;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.Socket;
import systems.zlink.contracts.sockets.SubmitResult;

class SubmitCloseRaceContractTest {
    private enum Operation {
        SEND,
        REQUEST,
        REPLY
    }

    @Test
    void replyRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.REPLY);
    }

    @Test
    void sendRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.SEND);
    }

    @Test
    void requestRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.REQUEST);
    }

    private static SubmitResult submit(Operation operation, RouterSocket router,
        DealerSocket dealer, Received inbound, Message message) {
        return switch (operation) {
            case SEND -> router.send(inbound.getRoutingId().orElseThrow())
                .message(message).submit().result();
            case REQUEST -> dealer.request().message(message)
                .timeout(Duration.ofSeconds(5)).submit().result();
            case REPLY -> {
                inbound.reply().message(message).submit();
                yield SubmitResult.OK;
            }
        };
    }

    private static void race(Operation operation) throws Exception {
        TestSupport.assumeNative();
        int rounds = 1_000;
        int submitters = 4;
        Map<String, Integer> outcomes = new TreeMap<>();
        Map<String, Integer> closes = new TreeMap<>();
        Throwable firstFailure = null;
        try (Context context = Zlink.createContext();
             var workers = Executors.newFixedThreadPool(submitters + 1)) {
            for (int round = 0; round < rounds; round++) {
                List<Received> received = new ArrayList<>();
                try (RouterSocket router = context.createRouterSocket();
                     DealerSocket dealer = context.createDealerSocket();
                     Poller poller = Zlink.createPoller()) {
                    String endpoint = TestSupport.inprocEndpoint(
                        "submit-close");
                    router.bind(endpoint);
                    dealer.connect(endpoint);
                    poller.add(dealer, 1, PollEventFlags.POLLCOMPLETION);
                    for (int index = 0; index < submitters; index++) {
                        try (Message request = Message.from("request")) {
                            dealer.request().message(request)
                                .timeout(Duration.ofSeconds(5)).submit();
                        }
                        Received inbound = new Received();
                        received.add(inbound);
                        assertTrue(router.recv(inbound, RecvFlags.NONE));
                    }
                    Socket closing = operation == Operation.REQUEST
                        ? dealer : router;
                    CountDownLatch ready = new CountDownLatch(submitters + 1);
                    CountDownLatch start = new CountDownLatch(1);
                    List<Future<Throwable>> submissions = new ArrayList<>();
                    for (Received inbound : received) {
                        submissions.add(workers.submit(() -> {
                            try (Message message = Message.from(
                                "racing-submit")) {
                                ready.countDown();
                                assertTrue(start.await(
                                    TestSupport.DEFAULT_TIMEOUT_MS,
                                    TimeUnit.MILLISECONDS));
                                assertEquals(SubmitResult.OK, submit(operation,
                                    router, dealer, inbound, message));
                                return null;
                            } catch (RuntimeException failure) {
                                return failure;
                            }
                        }));
                    }
                    Future<String> close = workers.submit(() -> {
                        ready.countDown();
                        assertTrue(start.await(TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS));
                        try {
                            closing.close();
                            return "OK";
                        } catch (ZlinkCloseException failure) {
                            assertEquals(CloseResult.BUSY, failure.getResult());
                            return "BUSY";
                        }
                    });
                    try {
                        assertTrue(ready.await(TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS));
                    } finally {
                        start.countDown();
                    }
                    for (Future<Throwable> submission : submissions) {
                        Throwable failure = submission.get(
                            TestSupport.DEFAULT_TIMEOUT_MS,
                            TimeUnit.MILLISECONDS);
                        String outcome = failure == null ? "OK"
                            : failure instanceof ZlinkSubmitException submit
                                ? submit.getResult() + "/"
                                    + submit.getNativeErrno()
                                : failure.getClass().getSimpleName();
                        outcomes.merge(outcome, 1, Integer::sum);
                        if (failure != null
                            && !(failure instanceof ZlinkSubmitException submit
                                && submit.getResult()
                                    == SubmitResult.TERMINATED)
                            && firstFailure == null) {
                            firstFailure = failure;
                        }
                    }
                    closes.merge(close.get(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS), 1, Integer::sum);
                    // A BUSY close left the socket open. Resource cleanup
                    // closes it after all submits return, without resubmission.
                } finally {
                    received.forEach(Received::close);
                }
            }
        }
        System.out.println(operation + " rounds=" + rounds + " submissions="
            + (rounds * submitters) + " outcomes=" + outcomes
            + " closes=" + closes);
        assertNull(firstFailure,
            () -> "unexpected submit outcome: " + outcomes);
    }
}
