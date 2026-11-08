/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

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
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.PubSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.NativeErrno;

class PublishCloseRaceContractTest {
    private enum Operation {
        PUBLISH,
        PUBLISH_NO_WAIT,
        FRAME,
        FRAME_NO_WAIT
    }

    @Test
    void publishRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.PUBLISH);
    }

    @Test
    void publishNoWaitRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.PUBLISH_NO_WAIT);
    }

    @Test
    void frameRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.FRAME);
    }

    @Test
    void frameNoWaitRacingCloseNeverUsesReleasedHandle() throws Exception {
        race(Operation.FRAME_NO_WAIT);
    }

    private static void publish(Operation operation, PubSocket pub,
        Message first, Message second) {
        switch (operation) {
            case PUBLISH -> pub.publish("race").message(first).message(second)
                .submit();
            case PUBLISH_NO_WAIT -> pub.publish("race").message(first)
                .message(second).flags(SendFlags.DONT_WAIT).submit();
            case FRAME -> ((NativeSocketBase) pub).runtime()
                .publishMessageFrame("race", first, SendFlag.NONE);
            case FRAME_NO_WAIT -> assertEquals(SendResult.SENT,
                ((NativeSocketBase) pub).runtime()
                    .publishMessageFrameNoWaitResult("race", first));
        }
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
                try (PubSocket pub = context.createPubSocket()) {
                    pub.bind(TestSupport.inprocEndpoint("publish-close"));
                    CountDownLatch ready = new CountDownLatch(submitters + 1);
                    CountDownLatch start = new CountDownLatch(1);
                    List<Future<Throwable>> submissions = new ArrayList<>();
                    for (int index = 0; index < submitters; index++) {
                        submissions.add(workers.submit(() -> {
                            try (Message first = Message.from("first");
                                 Message second = Message.from("second")) {
                                ready.countDown();
                                assertTrue(start.await(
                                    TestSupport.DEFAULT_TIMEOUT_MS,
                                    TimeUnit.MILLISECONDS));
                                publish(operation, pub, first, second);
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
                            pub.close();
                            return "OK";
                        } catch (ZlinkCloseException failure) {
                            assertEquals(CloseResult.BUSY, failure.getResult());
                            assertEquals(NativeErrno.EBUSY,
                                failure.getNativeErrno());
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
                                    == SubmitResult.TERMINATED
                                && submit.getNativeErrno()
                                    == NativeErrno.ESHUTDOWN)
                            && firstFailure == null) {
                            firstFailure = failure;
                        }
                    }
                    closes.merge(close.get(TestSupport.DEFAULT_TIMEOUT_MS,
                        TimeUnit.MILLISECONDS), 1, Integer::sum);
                    // Cleanup closes a BUSY socket after all publishes return.
                }
            }
        }
        System.out.println(operation + " rounds=" + rounds + " submissions="
            + (rounds * submitters) + " outcomes=" + outcomes
            + " closes=" + closes);
        assertNull(firstFailure,
            () -> "unexpected publish outcome: " + outcomes);
    }
}
