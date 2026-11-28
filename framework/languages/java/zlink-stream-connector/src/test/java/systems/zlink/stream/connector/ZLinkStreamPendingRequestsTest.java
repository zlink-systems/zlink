package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.function.BiFunction;
import java.util.function.BooleanSupplier;

final class ZLinkStreamPendingRequestsTest {
    @Test
    void cancellationRemovesPendingCorrelationBeforeALateReply() {
        var pendingRequests = pendingRequests();
        var pending =
                addAttempt(
                        pendingRequests,
                        7L,
                        "Echo",
                        (reply, complete) -> complete.getAsBoolean(),
                        (failure, complete) -> complete.getAsBoolean());

        assertTrue(pending.cancel(false));
        assertFalse(pendingRequests.fail(7L, new IllegalStateException("late failure")));

        pendingRequests.complete(
                7L,
                () -> new ZLinkStreamEncodedPayload("Echo", Message.from("late reply"), Map.of()));
    }

    static ZLinkStreamPendingRequests pendingRequests() {
        return pendingRequests(ZLinkStreamExecutionResources.timeouts(), System::nanoTime);
    }

    static ZLinkStreamPendingRequests pendingRequests(
            java.util.concurrent.ScheduledExecutorService scheduler,
            java.util.function.LongSupplier clock) {
        var queue =
                new ZLinkStreamAdmissionQueue(
                        scheduler,
                        () -> 0,
                        (owner, sequence, canWrite) -> fail("unexpected admission enqueue"),
                        () -> fail("unexpected queued event"),
                        position -> fail("unexpected position event"));
        return new ZLinkStreamPendingRequests(scheduler, clock, () -> 0, queue);
    }

    static CompletableFuture<ZLinkStreamEncodedPayload> addAttempt(
            ZLinkStreamPendingRequests requests,
            long sequence,
            String packetName,
            BiFunction<ZLinkStreamEncodedPayload, BooleanSupplier, Boolean> onReply,
            BiFunction<Throwable, BooleanSupplier, Boolean> onFailure) {
        var request =
                requests.start(
                        null,
                        packetName,
                        Duration.ofSeconds(30),
                        null,
                        onReply,
                        onFailure,
                        ignored -> {},
                        ignored -> {});
        return request.enqueueAttempt(sequence, ignored -> new CompletableFuture<>());
    }
}
