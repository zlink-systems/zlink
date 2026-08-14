/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.foreign.MemorySegment;
import java.time.Duration;
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
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SubmitResult;

class RoutedAdmissionTest {
    private final ManualExecutor executor = new ManualExecutor();
    private final ScheduledThreadPoolExecutor deadlines =
        new ScheduledThreadPoolExecutor(1, runnable -> {
            Thread thread = new Thread(runnable,
                "routed-admission-test-deadline");
            thread.setDaemon(true);
            return thread;
        });
    private final FakeReplies replies = new FakeReplies();

    @AfterEach
    void stopDeadlines() {
        deadlines.shutdownNow();
    }

    @Test
    void completionExistsBeforeSelectionOrFirstAttempt() {
        RoutingId rid = RoutingId.from("target");
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid, 1, 1);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, target));
        nativeAccess.results(target, SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.send(rid, onePart(), -1)
            .toCompletableFuture();

        assertFalse(result.isDone());
        assertEquals(0, nativeAccess.selectCount);
        assertEquals(0, nativeAccess.attempts.size());

        executor.runNext();
        assertEquals(1, nativeAccess.selectCount);
        assertEquals(0, nativeAccess.attempts.size());
        assertFalse(result.isDone());

        executor.runNext();
        assertTrue(result.isDone());
        assertEquals(List.of(target), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void blockedTargetDoesNotDelayAnotherTargetAndReadyIsDeduplicated() {
        RoutingId ridA = RoutingId.from("a");
        RoutingId ridB = RoutingId.from("b");
        RoutedAdmission.Target targetA = new RoutedAdmission.Target(ridA, 1, 1);
        RoutedAdmission.Target targetB = new RoutedAdmission.Target(ridB, 2, 1);
        FakeNative nativeAccess = new FakeNative(Map.of(
            ridA, targetA, ridB, targetB));
        nativeAccess.results(targetA, SubmitResult.BACKPRESSURED,
            SubmitResult.OK, SubmitResult.OK);
        nativeAccess.results(targetB, SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> firstA = admission.send(ridA, onePart(), -1)
            .toCompletableFuture();
        CompletableFuture<Void> secondA = admission.send(ridA, onePart(), -1)
            .toCompletableFuture();
        CompletableFuture<Void> firstB = admission.send(ridB, onePart(), -1)
            .toCompletableFuture();

        executor.runNext();
        executor.runNext();
        executor.runNext();
        executor.runNext();

        assertFalse(firstA.isDone());
        assertFalse(secondA.isDone());
        assertTrue(firstB.isDone());
        assertEquals(List.of(targetA, targetB), nativeAccess.attempts);

        admission.onReady(targetA, RoutedAdmission.ROUTED_WRITABLE, 0);
        admission.onReady(targetA, RoutedAdmission.ROUTED_WRITABLE, 0);
        assertEquals(1, executor.size());
        executor.runNext();

        assertTrue(firstA.isDone());
        assertTrue(secondA.isDone());
        assertEquals(List.of(targetA, targetB, targetA, targetA),
            nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void staleGenerationAndCancellationCannotRetryTheOperation() {
        RoutingId rid = RoutingId.from("target");
        RoutedAdmission.Target current = new RoutedAdmission.Target(rid, 1, 7);
        RoutedAdmission.Target stale = new RoutedAdmission.Target(rid, 1, 6);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, current));
        nativeAccess.results(current, SubmitResult.BACKPRESSURED,
            SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.send(rid, onePart(), -1)
            .toCompletableFuture();
        executor.runNext();
        executor.runNext();
        assertEquals(1, nativeAccess.attempts.size());

        admission.onReady(stale, RoutedAdmission.ROUTED_WRITABLE, 0);
        assertEquals(0, executor.size());
        assertTrue(result.cancel(false));
        admission.onReady(current, RoutedAdmission.ROUTED_WRITABLE, 0);
        executor.runAll();

        assertTrue(result.isCancelled());
        assertEquals(1, nativeAccess.attempts.size());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void zeroTimeoutQueuedBehindTheSameTargetGetsNoLateFirstAttempt()
        throws Exception {
        RoutingId rid = RoutingId.from("zero-timeout");
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid, 3, 1);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, target));
        nativeAccess.results(target, SubmitResult.BACKPRESSURED,
            SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> front = admission.send(rid, onePart(), -1)
            .toCompletableFuture();
        CompletableFuture<Void> queued = admission.send(rid, onePart(), 0)
            .toCompletableFuture();
        executor.runNext();
        executor.runNext();
        executor.runAll();

        org.junit.jupiter.api.Assertions.assertThrows(ExecutionException.class,
            () -> queued.get(1, TimeUnit.SECONDS));
        assertFalse(front.isDone());
        assertEquals(List.of(target), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void failedNativeCloseCanResumeTheSamePendingOperation() {
        RoutingId rid = RoutingId.from("close-retry");
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid, 5, 4);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, target));
        nativeAccess.results(target, SubmitResult.BACKPRESSURED,
            SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.send(rid, onePart(), -1)
            .toCompletableFuture();
        executor.runAll();
        assertFalse(result.isDone());

        admission.prepareClose();
        assertFalse(result.isDone());
        admission.onReady(target, RoutedAdmission.ROUTED_WRITABLE, 0);
        assertEquals(0, executor.size());
        admission.abortClose();
        executor.runAll();

        assertTrue(result.isDone());
        assertEquals(List.of(target, target), nativeAccess.attempts);
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void originalDeadlineExpiresWithoutReadinessOrRetryPolling()
        throws Exception {
        RoutingId rid = RoutingId.from("deadline");
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid, 4, 2);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, target));
        nativeAccess.results(target, SubmitResult.BACKPRESSURED,
            SubmitResult.OK);
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<Void> result = admission.send(rid, onePart(), 20)
            .toCompletableFuture();
        executor.runNext();
        executor.runNext();

        org.junit.jupiter.api.Assertions.assertThrows(ExecutionException.class,
            () -> result.get(1, TimeUnit.SECONDS));
        admission.onReady(target, RoutedAdmission.ROUTED_WRITABLE, 0);
        executor.runAll();
        assertEquals(1, nativeAccess.attempts.size());
        admission.beginClose();
        admission.finishClose();
    }

    @Test
    void fastReplyCanCompleteDuringTheFirstAcceptedAttempt() {
        RoutingId rid = RoutingId.from("reply");
        RoutedAdmission.Target target = new RoutedAdmission.Target(rid, 9, 3);
        FakeNative nativeAccess = new FakeNative(Map.of(rid, target));
        nativeAccess.results(target, SubmitResult.OK);
        nativeAccess.fastReply = true;
        RoutedAdmission admission = admission(nativeAccess);

        CompletableFuture<List<Message>> result = admission.request(target,
            onePart(), Duration.ofSeconds(1)).toCompletableFuture();
        assertFalse(result.isDone());
        executor.runNext();
        executor.runNext();

        assertTrue(result.isDone());
        assertEquals(List.of(), result.join());
        assertEquals(1, replies.registrationCount.get());
        admission.beginClose();
        admission.finishClose();
    }

    private RoutedAdmission admission(FakeNative nativeAccess) {
        return new RoutedAdmission(nativeAccess, replies, executor, deadlines);
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

        private int size() {
            return work.size();
        }

        private void runNext() {
            work.removeFirst().run();
        }

        private void runAll() {
            while (!work.isEmpty())
                runNext();
        }
    }

    private static final class FakeNative
      implements RoutedAdmission.NativeAccess {
        private final Map<RoutingId, RoutedAdmission.Target> targets;
        private final Map<RoutedAdmission.Target, ArrayDeque<SubmitResult>>
            results = new HashMap<>();
        private final List<RoutedAdmission.Target> attempts = new ArrayList<>();
        private int selectCount;
        private boolean fastReply;

        private FakeNative(Map<RoutingId, RoutedAdmission.Target> targets) {
            this.targets = targets;
        }

        private void results(RoutedAdmission.Target target,
                             SubmitResult... values) {
            results.put(target, new ArrayDeque<>(List.of(values)));
        }

        @Override
        public RoutedAdmission.Selection select(RoutingId selector) {
            selectCount++;
            return new RoutedAdmission.Selection(SubmitResult.OK,
                targets.get(selector), 0);
        }

        @Override
        public RoutedAdmission.AttemptResult send(
                RoutedAdmission.Target target, List<Message> parts) {
            attempts.add(target);
            return new RoutedAdmission.AttemptResult(
                results.get(target).removeFirst(), 0);
        }

        @Override
        public RoutedAdmission.AttemptResult request(
                RoutedAdmission.Target target, List<Message> parts,
                int timeoutMs, long requestId,
                RoutedAdmission.ReplyRegistry replies) {
            RoutedAdmission.AttemptResult result = send(target, parts);
            if (fastReply && result.result() == SubmitResult.OK)
                ((FakeReplies) replies).complete(requestId);
            return result;
        }
    }

    private static final class FakeReplies
      implements RoutedAdmission.ReplyRegistry {
        private long nextId;
        private final Map<Long, CompletableFuture<List<Message>>> pending =
            new HashMap<>();
        private final AtomicInteger registrationCount = new AtomicInteger();

        @Override
        public long nextRequestId() {
            return ++nextId;
        }

        @Override
        public void register(long requestId,
                             CompletableFuture<List<Message>> future) {
            pending.put(requestId, future);
            registrationCount.incrementAndGet();
        }

        @Override
        public void remove(long requestId) {
            pending.remove(requestId);
        }

        @Override
        public MemorySegment callback() {
            return MemorySegment.NULL;
        }

        @Override
        public MemorySegment userData(long requestId) {
            return MemorySegment.NULL;
        }

        private void complete(long requestId) {
            pending.get(requestId).complete(List.of());
        }
    }
}
