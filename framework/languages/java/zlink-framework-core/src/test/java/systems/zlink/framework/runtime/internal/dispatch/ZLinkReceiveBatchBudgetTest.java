package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ZLinkReceiveBatchBudgetTest {
    @Test
    void firstOversizedRecordIsAdmittedButTheNextRecordIsHeld() {
        ZLinkReceiveBatchBudget budget =
            new ZLinkReceiveBatchBudget(8, 4, 1_000_000_000L);

        assertTrue(budget.canReceiveNext());
        budget.record(7);
        assertFalse(budget.canReceiveNext());
        assertEquals(1, budget.messageCount());
        assertEquals(7, budget.byteCount());
    }

    @Test
    void messageLimitStopsTheBatch() {
        ZLinkReceiveBatchBudget budget =
            new ZLinkReceiveBatchBudget(2, 100, 1_000_000_000L);

        budget.record(1);
        assertTrue(budget.canReceiveNext());
        budget.record(1);
        assertFalse(budget.canReceiveNext());
    }

    @Test
    void byteAccountingUsesMessageSizesWithoutReadingPayloads() {
        try (Message first = Message.from(new byte[3]);
             Message second = Message.from(new byte[5])) {
            assertEquals(
                8,
                ZLinkReceiveBatchBudget.bytesOf(List.of(first, second)));
        }
    }
}
