/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.integration.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.PairSocket;
import systems.zlink.contracts.sockets.RecvFlags;

public class SocketConcurrencyStressTest {
    private static final int SENDER_THREADS = 4;
    // Pull completion performs an actual completion drain for every pending
    // admission. Keep this a close/register race test rather than a benchmark;
    // the perf projects own sustained-throughput coverage.
    private static final int ATTEMPTS_PER_SENDER = 1_000;
    private static final int CLOSE_AFTER_ATTEMPTS = 2_000;

    @Test
    public void concurrentSingleAndMultipartSendMixedWithClose()
        throws Exception {
        TestSupport.assumeNative();

        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger returnedStages = new AtomicInteger();
        AtomicInteger completions = new AtomicInteger();
        AtomicInteger receivedRecords = new AtomicInteger();
        AtomicBoolean sendersDone = new AtomicBoolean();
        AtomicBoolean closeStarted = new AtomicBoolean();
        AtomicBoolean closeAccepted = new AtomicBoolean();
        AtomicReference<Throwable> receiverFailure = new AtomicReference<>();
        CountDownLatch start = new CountDownLatch(1);

        try (Context context = Zlink.createContext();
             PairSocket sender = context.createPairSocket();
             PairSocket receiver = context.createPairSocket();
             ExecutorService workers = Executors.newFixedThreadPool(
                 SENDER_THREADS + 2)) {
            String endpoint = TestSupport.inprocEndpoint(
                "socket-concurrency-stress");
            sender.bind(endpoint);
            receiver.connect(endpoint);

            Future<?> receiverTask = workers.submit(() -> {
                start.await();
                int idlePasses = 0;
                try (Received received = new Received()) {
                    while (!sendersDone.get() || idlePasses < 1_000) {
                        if (!receiver.recv(received, RecvFlags.DONT_WAIT)) {
                            idlePasses++;
                            Thread.onSpinWait();
                            continue;
                        }
                        idlePasses = 0;
                        assertRecordPartsStayAdjacent(received.parts());
                        receivedRecords.incrementAndGet();
                    }
                } catch (Throwable failure) {
                    receiverFailure.compareAndSet(null, failure);
                }
                return null;
            });

            Future<?> closeTask = workers.submit(() -> {
                start.await();
                while (attempts.get() < CLOSE_AFTER_ATTEMPTS) {
                    Thread.onSpinWait();
                }
                closeStarted.set(true);
                long deadline = System.nanoTime()
                    + TimeUnit.MILLISECONDS.toNanos(
                        TestSupport.DEFAULT_TIMEOUT_MS);
                while (!closeAccepted.get() && System.nanoTime() < deadline) {
                    try {
                        sender.close();
                        closeAccepted.set(true);
                    } catch (ZlinkCloseException failure) {
                        if (failure.getResult() != CloseResult.BUSY) {
                            throw failure;
                        }
                        Thread.onSpinWait();
                    }
                }
                assertTrue(closeAccepted.get(), "close remained BUSY");
                return null;
            });

            Future<?>[] senderTasks = new Future<?>[SENDER_THREADS];
            for (int thread = 0; thread < SENDER_THREADS; thread++) {
                int threadId = thread;
                senderTasks[thread] = workers.submit(() -> {
                    start.await();
                    for (int iteration = 0;
                         iteration < ATTEMPTS_PER_SENDER; iteration++) {
                        attempts.incrementAndGet();
                        submitRecord(sender, threadId, iteration,
                            returnedStages, completions, closeStarted);
                    }
                    return null;
                });
            }

            start.countDown();
            for (Future<?> senderTask : senderTasks) {
                senderTask.get(TestSupport.DEFAULT_TIMEOUT_MS * 4L,
                    TimeUnit.MILLISECONDS);
            }
            sendersDone.set(true);
            closeTask.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);
            receiverTask.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS);

            if (receiverFailure.get() != null) {
                throw new AssertionError("receiver failed", receiverFailure.get());
            }
            assertEquals(SENDER_THREADS * ATTEMPTS_PER_SENDER,
                attempts.get());
            assertEquals(returnedStages.get(), completions.get(),
                "every returned stage must complete exactly once");
            assertTrue(receivedRecords.get() > 0,
                "stress must exercise admitted sends before close");
        }
    }

    private static void submitRecord(PairSocket sender, int threadId,
                                     int iteration,
                                     AtomicInteger returnedStages,
                                     AtomicInteger completions,
                                     AtomicBoolean closeStarted)
        throws Exception {
        boolean multipart = (iteration & 1) != 0;
        String recordId = threadId + ":" + iteration;
        try (Message first = Message.from(
                 (multipart ? "m:" : "s:") + recordId + ":0");
             Message second = multipart
                 ? Message.from("m:" + recordId + ":1") : null;
             Message third = multipart
                 ? Message.from("m:" + recordId + ":2") : null) {
            CompletableFuture<Void> completion;
            try {
                var operation = sender.send().message(first);
                if (multipart) {
                    operation = operation.message(second).message(third);
                }
                completion = operation.submit().toCompletableFuture();
            } catch (ZlinkSubmitException | IllegalStateException failure) {
                if (!closeStarted.get()
                    && !(failure instanceof ZlinkSubmitException)) {
                    throw failure;
                }
                return;
            }

            returnedStages.incrementAndGet();
            CompletableFuture<Void> observed = completion.whenComplete(
                (ignored, failure) -> completions.incrementAndGet());
            try {
                observed.get(TestSupport.DEFAULT_TIMEOUT_MS,
                    TimeUnit.MILLISECONDS);
            } catch (ExecutionException failure) {
                if (!(failure.getCause() instanceof ZlinkSubmitException)) {
                    throw failure;
                }
            }
        }
    }

    private static void assertRecordPartsStayAdjacent(List<Message> parts) {
        assertTrue(parts.size() == 1 || parts.size() == 3,
            "received a partial multipart record");
        String first = parts.getFirst().toUtf8String();
        if (parts.size() == 1) {
            assertTrue(first.startsWith("s:"));
            return;
        }
        String recordId = first.substring(2, first.length() - 2);
        assertEquals("m:" + recordId + ":0", first);
        assertEquals("m:" + recordId + ":1",
            parts.get(1).toUtf8String());
        assertEquals("m:" + recordId + ":2",
            parts.get(2).toUtf8String());
    }
}
