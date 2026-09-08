package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiRoutedReplyQueueTest {
    private static PerfMultiRoutedReplyQueue<Integer> queue(
            PerfMultiRoutedReplyQueue.Submitter<Integer> submitter,
            List<Integer> released) {
        return new PerfMultiRoutedReplyQueue<>(submitter, released::add,
            cause -> false);
    }

    @Test
    void oneReplyIsSubmittedUntilThePreviousAdmissionCompletes() {
        List<Integer> submitted = new ArrayList<>();
        List<CompletableFuture<Void>> stages = new ArrayList<>();
        var replies = queue(reply -> {
            submitted.add(reply);
            CompletableFuture<Void> stage = new CompletableFuture<>();
            stages.add(stage);
            return stage;
        }, new ArrayList<>());

        replies.enqueue(1);
        replies.enqueue(2);
        replies.enqueue(3);
        assertEquals(List.of(1), submitted,
            "the sender must await the preceding admission");
        assertEquals(2, replies.pendingCount());
        assertTrue(replies.sending());

        stages.get(0).complete(null);
        assertEquals(List.of(1, 2), submitted);
        stages.get(1).complete(null);
        stages.get(2).complete(null);
        assertEquals(List.of(1, 2, 3), submitted,
            "the FIFO preserves the received order");
        assertEquals(0, replies.pendingCount());
        assertFalse(replies.sending());
        assertNull(replies.failure());
    }

    @Test
    void immediateAdmissionDrainsADeepFifoWithoutRecursion() {
        int depth = 50_000;
        List<Integer> submitted = new ArrayList<>();
        CompletableFuture<Void> first = new CompletableFuture<>();
        var replies = queue(reply -> {
            submitted.add(reply);
            return reply == 0 ? first : CompletableFuture.completedStage(null);
        }, new ArrayList<>());

        for (int index = 0; index < depth; index++) {
            replies.enqueue(index);
        }
        assertEquals(List.of(0), submitted,
            "the FIFO waits behind the first unadmitted reply");

        // Core settles an immediate admission inline on the releasing thread.
        // The whole backlog must unwind on one stack frame, not one per reply.
        first.complete(null);
        assertEquals(depth, submitted.size());
        assertEquals(0, replies.pendingCount());
        assertFalse(replies.sending());
        assertNull(replies.failure());
    }

    @Test
    void backpressureIsNotAFailureAndTheRetryStageReleasesTheNextReply() {
        // A routed echo reply that Core backpressures resolves through the
        // binding's WRITABLE retry. It must never end the relay.
        List<Integer> submitted = new ArrayList<>();
        List<CompletableFuture<Void>> stages = new ArrayList<>();
        var replies = queue(reply -> {
            submitted.add(reply);
            CompletableFuture<Void> stage = new CompletableFuture<>();
            stages.add(stage);
            return stage;
        }, new ArrayList<>());

        replies.enqueue(1);
        replies.enqueue(2);
        assertFalse(replies.hasFailure());
        assertFalse(replies.drain(Duration.ofMillis(20)),
            "an unadmitted reply must not be reported as drained");

        stages.get(0).complete(null);
        assertNull(replies.failure(),
            "backpressure resolved by the retry stage is not a failure");
    }

    @Test
    void ignorableFailureDropsOnlyThatReplyAndKeepsTheFifoMoving() {
        List<Integer> submitted = new ArrayList<>();
        List<Integer> released = new ArrayList<>();
        RuntimeException stale = new RuntimeException("stale route");
        var replies = new PerfMultiRoutedReplyQueue<Integer>(reply -> {
            submitted.add(reply);
            if (reply == 1) {
                throw stale;
            }
            return CompletableFuture.completedStage(null);
        }, released::add, cause -> cause == stale);

        replies.enqueue(1);
        replies.enqueue(2);
        assertEquals(List.of(1, 2), submitted,
            "a dropped reply must not pin the FIFO head");
        assertEquals(List.of(1), released,
            "the dropped reply is released exactly once");
        assertNull(replies.failure());
        assertTrue(replies.drain(Duration.ofMillis(20)));
    }

    @Test
    void terminalFailureStopsTheSenderAndReleasesTheRemainingReplies() {
        RuntimeException terminal = new RuntimeException("terminal");
        List<Integer> submitted = new ArrayList<>();
        List<Integer> released = new ArrayList<>();
        var replies = new PerfMultiRoutedReplyQueue<Integer>(reply -> {
            submitted.add(reply);
            CompletionStage<Void> stage = new CompletableFuture<>();
            ((CompletableFuture<Void>) stage).completeExceptionally(terminal);
            return stage;
        }, released::add, cause -> false);

        replies.enqueue(1);
        replies.enqueue(2);
        assertEquals(List.of(1), submitted,
            "a failed sender must not submit the rest of the FIFO");
        assertSame(terminal, replies.failure());
        assertFalse(replies.drain(Duration.ofMillis(20)));
        assertEquals(List.of(2), released);
    }
}
