package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkStreamWaiterTerminalTest {
    @Test
    void timedOutWaiterCannotConsumeFromItsPublicCompletion() {
        assertLaterPacketRetained(false);
    }

    @Test
    void cancelledWaiterCannotConsumeFromItsPublicCompletion() {
        assertLaterPacketRetained(true);
    }

    @Test
    void claimedPacketCannotBeReplacedByTimeoutOrCancellation() {
        var queue = new ZLinkStreamDispatchQueue();
        var source = queue.awaitMessage("Late", ignored -> true);
        var message =
                new ZLinkStreamMessage<>(
                        "Late",
                        new ZLinkStreamEncodedPayload("Late", Message.from("body"), Map.of()),
                        Map.<String, String>of());
        try {
            queue.addMessage(message, () -> null, () -> 0, false);
            assertFalse(source.completeExceptionally(new TimeoutException("controlled")));
            assertFalse(source.cancel(false));
            assertSame(message, source.join());
            assertEquals(1, queue.receivedCount("Late"));
        } finally {
            message.payload().payload().close();
        }
    }

    @Test
    void waiterEndingInsidePredicateLeavesPacketQueued() {
        var queue = new ZLinkStreamDispatchQueue();
        var source =
                new AtomicReference<
                        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>>>();
        source.set(
                queue.awaitMessage(
                        "Late",
                        ignored -> {
                            assertTrue(
                                    source.get()
                                            .completeExceptionally(
                                                    new TimeoutException("controlled")));
                            return true;
                        }));
        var message =
                new ZLinkStreamMessage<>(
                        "Late",
                        new ZLinkStreamEncodedPayload("Late", Message.from("body"), Map.of()),
                        Map.<String, String>of());
        try {
            queue.addMessage(message, () -> null, () -> 0, false);
            var next = queue.awaitMessage("Late", ignored -> true);
            assertTrue(next.isDone());
            assertSame(message, next.join());
            assertEquals(1, queue.receivedCount("Late"));
        } finally {
            message.payload().payload().close();
        }
    }

    private static void assertLaterPacketRetained(boolean cancel) {
        var queue = new ZLinkStreamDispatchQueue();
        var source = queue.awaitMessage("Late", ignored -> true);
        var publicResult =
                ZLinkStreamWaitFailure.asValidationFailure(source, "controlled observation");
        var message =
                new ZLinkStreamMessage<>(
                        "Late",
                        new ZLinkStreamEncodedPayload("Late", Message.from("body"), Map.of()),
                        Map.<String, String>of());
        var publish =
                publicResult.whenComplete(
                        (ignored, error) -> {
                            assertTrue(publicResult.isDone());
                            queue.addMessage(message, () -> null, () -> 0, false);
                        });
        try {
            // Complete the same source that orTimeout completes, without a clock or timer.
            if (cancel) {
                assertTrue(publicResult.cancel(false));
            } else {
                assertTrue(source.completeExceptionally(new TimeoutException("controlled")));
            }
            assertTrue(publish.isDone());
            assertEquals(1, queue.receivedCount("Late"));
            var next = queue.awaitMessage("Late", ignored -> true);
            try {
                assertTrue(next.isDone(), "completed waiter consumed the later packet");
                assertSame(message, next.join());
            } finally {
                next.cancel(false);
            }
        } finally {
            message.payload().payload().close();
        }
    }
}
