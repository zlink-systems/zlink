package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ZLinkStreamPendingRequestsTest {
    @Test
    void cancellationRemovesPendingCorrelationBeforeALateReply() {
        ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        try {
            ZLinkStreamPendingRequests pendingRequests = new ZLinkStreamPendingRequests();
            var pending = pendingRequests.add(
                7L,
                "Echo",
                Duration.ofSeconds(1),
                scheduler);

            assertTrue(pending.cancel(false));
            assertFalse(pendingRequests.fail(7L, new IllegalStateException("late failure")));

            pendingRequests.complete(
                7L,
                new ZLinkStreamEncodedPayload(
                    "Echo",
                    Message.from("late reply"),
                    Map.of()));
        } finally {
            scheduler.shutdownNow();
        }
    }
}
