package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkJavaAdmissionControlRetryQueueTest {
    @Test
    void coalescesSameConnectionAndCommandAndCopiesFrames() {
        var queue = new ZLinkJavaAdmissionControlRetryQueue(4, 64);
        RoutingId target = RoutingId.from("retry-target");
        byte[] first = new byte[] {1, 2};
        long firstVersion = queue.nextIntentVersion();
        assertTrue(queue.remember(
            target,
            "connection-a",
            2,
            List.of(first),
            firstVersion));
        first[0] = 9;

        byte[] replacement = new byte[] {3, 4, 5};
        long replacementVersion = queue.nextIntentVersion();
        assertTrue(queue.remember(
            target,
            "connection-a",
            2,
            List.of(replacement),
            replacementVersion));
        assertEquals(1, queue.count());
        assertEquals(3, queue.bytes());

        AtomicInteger attempts = new AtomicInteger();
        queue.flush(pending -> {
            attempts.incrementAndGet();
            assertEquals(target, pending.target());
            assertEquals("connection-a", pending.connectionId());
            assertEquals(2, pending.command());
            assertArrayEquals(
                new byte[] {3, 4, 5},
                pending.frames().getFirst());
            return ZLinkJavaAdmissionControlRetryQueue.RetryResult.ACCEPTED;
        });
        assertEquals(1, attempts.get());
        assertEquals(0, queue.count());
    }

    @Test
    void backpressureRetriesEachPeerOncePerReadyEdgeWithoutHeadOfLineLoop() {
        var queue = new ZLinkJavaAdmissionControlRetryQueue(4, 64);
        RoutingId first = RoutingId.from("retry-first");
        RoutingId second = RoutingId.from("retry-second");
        remember(queue, first, "connection-a", 2, new byte[] {1});
        remember(queue, second, "connection-b", 2, new byte[] {2});

        AtomicInteger firstAttempts = new AtomicInteger();
        AtomicInteger secondAttempts = new AtomicInteger();
        int attempted = queue.flush(pending -> {
            if (pending.target().equals(first)) {
                firstAttempts.incrementAndGet();
                return ZLinkJavaAdmissionControlRetryQueue.RetryResult
                    .BACKPRESSURED;
            }
            secondAttempts.incrementAndGet();
            return ZLinkJavaAdmissionControlRetryQueue.RetryResult.ACCEPTED;
        });

        assertEquals(2, attempted);
        assertEquals(1, firstAttempts.get());
        assertEquals(1, secondAttempts.get());
        assertEquals(1, queue.count());

        queue.flush(pending ->
            ZLinkJavaAdmissionControlRetryQueue.RetryResult.ACCEPTED);
        assertEquals(0, queue.count());
    }

    @Test
    void removesOnlyTheConnectionThatWasDisconnected() {
        var queue = new ZLinkJavaAdmissionControlRetryQueue(4, 64);
        RoutingId target = RoutingId.from("retry-target");
        remember(queue, target, "connection-a", 2, new byte[] {1});
        remember(queue, target, "connection-b", 2, new byte[] {2});

        queue.removeTargetConnection(target, "connection-a");
        assertEquals(1, queue.count());
        queue.flush(pending -> {
            assertEquals("connection-b", pending.connectionId());
            return ZLinkJavaAdmissionControlRetryQueue.RetryResult.ACCEPTED;
        });
        assertEquals(0, queue.count());
    }

    @Test
    void enforcesRecordAndByteBounds() {
        var queue = new ZLinkJavaAdmissionControlRetryQueue(1, 2);
        RoutingId first = RoutingId.from("retry-first");
        RoutingId second = RoutingId.from("retry-second");
        remember(queue, first, "connection-a", 2, new byte[] {1, 2});
        long version = queue.nextIntentVersion();
        assertFalse(queue.remember(
            second,
            "connection-b",
            2,
            List.of(new byte[] {3}),
            version));
        assertEquals(1, queue.count());
        assertEquals(1, queue.capacityRejectionCount());
    }

    private static void remember(
        ZLinkJavaAdmissionControlRetryQueue queue,
        RoutingId target,
        String connectionId,
        int command,
        byte[] frame) {
        long version = queue.nextIntentVersion();
        assertTrue(queue.remember(
            target,
            connectionId,
            command,
            List.of(frame),
            version));
    }
}
