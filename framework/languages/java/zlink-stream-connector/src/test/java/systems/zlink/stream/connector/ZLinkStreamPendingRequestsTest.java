package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

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
        return new ZLinkStreamPendingRequests(
                ZLinkStreamExecutionResources.timeouts(), System::nanoTime, () -> 0);
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
