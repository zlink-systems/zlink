/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendResult;

class PublisherAdmissionTest {
    private final ManualExecutor executor = new ManualExecutor();
    private final ScheduledThreadPoolExecutor deadlines =
        new ScheduledThreadPoolExecutor(1, runnable -> {
            Thread thread = new Thread(runnable,
                "publisher-admission-test-deadline");
            thread.setDaemon(true);
            return thread;
        });

    @AfterEach
    void stopDeadlines() {
        deadlines.shutdownNow();
    }

    @Test
    void completionExistsBeforeTheFirstNativeAttempt() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), -1).toCompletableFuture();

        assertFalse(result.isDone());
        assertEquals(List.of(), nativeAccess.attempts);
        executor.runAll();
        assertTrue(result.isDone());
        assertEquals(List.of("events"), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void aParkedRecordDoesNotDelayAFreshRecord() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("blocked", SendResult.BACKPRESSURED,
            SendResult.SENT);
        nativeAccess.results("fresh", SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> blocked = admission.publish(
            "blocked", onePart(), -1).toCompletableFuture();
        executor.runAll();
        assertFalse(blocked.isDone());

        CompletableFuture<Void> fresh = admission.publish(
            "fresh", onePart(), -1).toCompletableFuture();
        executor.runAll();
        assertTrue(fresh.isDone());
        assertFalse(blocked.isDone());
        assertEquals(List.of("blocked", "fresh"), nativeAccess.attempts);

        admission.signalReady();
        executor.runAll();
        assertTrue(blocked.isDone());
        assertEquals(List.of("blocked", "fresh", "blocked"),
            nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void cancellationCannotRetryOnALaterReadyEdge() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.BACKPRESSURED,
            SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), -1).toCompletableFuture();
        executor.runAll();
        assertTrue(result.cancel(false));

        admission.signalReady();
        executor.runAll();
        assertTrue(result.isCancelled());
        assertEquals(List.of("events"), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void deadlineTerminatesWithoutRetryPolling() throws Exception {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.BACKPRESSURED,
            SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), 20).toCompletableFuture();
        executor.runAll();
        assertThrows(ExecutionException.class,
            () -> result.get(1, TimeUnit.SECONDS));

        admission.signalReady();
        executor.runAll();
        assertEquals(List.of("events"), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void zeroTimeoutGetsOneImmediateAttemptAndNeverParks() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.BACKPRESSURED,
            SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), 0).toCompletableFuture();
        executor.runAll();
        assertTrue(result.isCompletedExceptionally());

        admission.signalReady();
        executor.runAll();
        assertEquals(List.of("events"), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void aReadyEdgeObservedDuringFailedCloseResumesAfterAbort() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.BACKPRESSURED,
            SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), -1).toCompletableFuture();
        executor.runAll();
        admission.prepareClose();
        admission.signalReady();
        admission.abortClose();
        executor.runAll();

        assertTrue(result.isDone());
        assertEquals(List.of("events", "events"), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void completionCallbackCanCloseWithoutDeadlockingThePump() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.SENT);
        PublisherAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), -1).toCompletableFuture();
        result.whenComplete((ignored, failure) -> {
            admission.beginClose();
            admission.finishClose();
        });
        executor.runAll();

        assertTrue(result.isDone());
        assertEquals(List.of("events"), nativeAccess.attempts);
    }

    @Test
    void closeTerminatesParkedRecordsAndPreservesTheObserverSlot() {
        FakeNative nativeAccess = new FakeNative();
        nativeAccess.results("events", SendResult.BACKPRESSURED);
        PublisherAdmission admission = admission(nativeAccess);
        AtomicInteger readyCalls = new AtomicInteger();
        admission.setObserver(readyCalls::incrementAndGet);

        CompletableFuture<Void> result = admission.publish(
            "events", onePart(), -1).toCompletableFuture();
        executor.runAll();
        admission.signalReady();
        assertEquals(1, readyCalls.get());

        admission.beginClose();
        assertTrue(result.isCompletedExceptionally());
        admission.finishClose();
    }

    private PublisherAdmission admission(FakeNative nativeAccess) {
        return new PublisherAdmission(nativeAccess, executor, deadlines);
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
            while (!work.isEmpty())
                work.removeFirst().run();
        }
    }

    private static final class FakeNative
      implements PublisherAdmission.NativeAccess {
        private final Map<String, ArrayDeque<SendResult>> results =
            new HashMap<>();
        private final List<String> attempts = new ArrayList<>();

        private void results(String topic, SendResult... values) {
            results.put(topic, new ArrayDeque<>(List.of(values)));
        }

        @Override
        public SendResult publish(String topic, List<Message> parts) {
            attempts.add(topic);
            return results.get(topic).removeFirst();
        }
    }
}
