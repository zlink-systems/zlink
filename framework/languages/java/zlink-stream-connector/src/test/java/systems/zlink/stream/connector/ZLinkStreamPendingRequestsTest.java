package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.util.Map;

final class ZLinkStreamPendingRequestsTest {
    @Test
    void cancellationRemovesPendingCorrelationBeforeALateReply() {
        ZLinkStreamPendingRequests pendingRequests = new ZLinkStreamPendingRequests();
        var pending = pendingRequests.add(7L, "Echo");

        assertTrue(pending.cancel(false));
        assertFalse(pendingRequests.fail(7L, new IllegalStateException("late failure")));

        pendingRequests.complete(
                7L,
                () -> new ZLinkStreamEncodedPayload("Echo", Message.from("late reply"), Map.of()));
    }
}
