/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayDeque;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendResult;

final class StreamAdmissionTest {
    private static final RoutingId PEER = RoutingId.from("stream-peer");
    private final ManualExecutor executor = new ManualExecutor();
    private final ScheduledThreadPoolExecutor deadlines =
        new ScheduledThreadPoolExecutor(1, runnable -> {
            Thread thread = new Thread(runnable,
                "stream-admission-test-deadline");
            thread.setDaemon(true);
            return thread;
        });

    @AfterEach
    void stopDeadlines() {
        deadlines.shutdownNow();
    }

    @Test
    void completionExistsBeforeTheImmediateAttempt() {
        FakeNative nativeAccess = new FakeNative(SendResult.SENT);
        StreamAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.send(
            PEER, onePart(), -1).toCompletableFuture();

        assertFalse(result.isDone());
        assertEquals(0, nativeAccess.attempts.get());
        executor.runAll();
        assertTrue(result.isDone());
        assertEquals(1, nativeAccess.attempts.get());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void sendReadyResumesOneParkedPhysicalRecord() {
        FakeNative nativeAccess = new FakeNative(
            SendResult.BACKPRESSURED, SendResult.SENT);
        StreamAdmission admission = admission(nativeAccess);
        CompletableFuture<Void> result = admission.send(
            PEER, onePart(), -1).toCompletableFuture();

        executor.runAll();
        assertFalse(result.isDone());
        assertEquals(1, nativeAccess.attempts.get());
        admission.signalReady();
        executor.runAll();

        assertTrue(result.isDone());
        assertEquals(2, nativeAccess.attempts.get());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void timeoutTerminatesOnceWithoutPollingOrLaterRetry() throws Exception {
        FakeNative nativeAccess = new FakeNative(
            SendResult.BACKPRESSURED, SendResult.SENT);
        StreamAdmission admission = admission(nativeAccess);
        CompletableFuture<Void> result = admission.send(
            PEER, onePart(), 20).toCompletableFuture();

        executor.runAll();
        assertThrows(ExecutionException.class,
            () -> result.get(1, TimeUnit.SECONDS));
        admission.signalReady();
        executor.runAll();

        assertEquals(1, nativeAccess.attempts.get());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void cancellationTerminatesOnceAndIgnoresLaterReady() {
        FakeNative nativeAccess = new FakeNative(
            SendResult.BACKPRESSURED, SendResult.SENT);
        StreamAdmission admission = admission(nativeAccess);
        CompletableFuture<Void> result = admission.send(
            PEER, onePart(), -1).toCompletableFuture();
        executor.runAll();

        assertTrue(result.cancel(false));
        admission.signalReady();
        executor.runAll();

        assertTrue(result.isCancelled());
        assertEquals(1, nativeAccess.attempts.get());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void closeTerminatesParkedRecordExactlyOnceAndPreservesObserver() {
        FakeNative nativeAccess = new FakeNative(SendResult.BACKPRESSURED);
        StreamAdmission admission = admission(nativeAccess);
        AtomicInteger readyCalls = new AtomicInteger();
        admission.setObserver(readyCalls::incrementAndGet);
        CompletableFuture<Void> result = admission.send(
            PEER, onePart(), -1).toCompletableFuture();
        executor.runAll();

        admission.signalReady();
        assertEquals(1, readyCalls.get());
        admission.beginClose();
        admission.beginClose();

        assertTrue(result.isCompletedExceptionally());
        assertEquals(1, nativeAccess.attempts.get());
        admission.finishClose();
    }

    private StreamAdmission admission(FakeNative nativeAccess) {
        return new StreamAdmission(nativeAccess, executor, deadlines);
    }

    private static List<Message> onePart() {
        return Collections.singletonList(null);
    }

    private static final class ManualExecutor implements Executor {
        private final ArrayDeque<Runnable> work = new ArrayDeque<>();

        @Override
        public void execute(Runnable command) {
            work.addLast(command);
        }

        private void runAll() {
            while (!work.isEmpty()) {
                work.removeFirst().run();
            }
        }
    }

    private static final class FakeNative
        implements StreamAdmission.NativeAccess {
        private final ArrayDeque<SendResult> results;
        private final AtomicInteger attempts = new AtomicInteger();

        private FakeNative(SendResult... results) {
            this.results = new ArrayDeque<>(List.of(results));
        }

        @Override
        public SendResult send(RoutingId routingId, List<Message> parts) {
            attempts.incrementAndGet();
            return results.removeFirst();
        }
    }
}
