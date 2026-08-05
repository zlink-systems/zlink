package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;

import java.util.List;
import org.junit.jupiter.api.Test;

final class ZLinkServiceMailboxCloseTest {
    @Test
    void closeReleasesAllRetainedQueueRecords() {
        ZLinkServiceMailbox mailbox =
            new ZLinkServiceMailbox(4, 1024, 4, 1024);
        assertFalse(mailbox.tryEnqueue(new ZLinkServiceMailbox.Record(
            "owner",
            ZLinkServiceMailbox.Domain.APPLICATION,
            List.of(new byte[2048]),
            null,
            null,
            null)));
        mailbox.tryEnqueue(new ZLinkServiceMailbox.Record(
            "owner",
            ZLinkServiceMailbox.Domain.APPLICATION,
            List.of(new byte[32]),
            null,
            null,
            null));
        mailbox.tryEnqueue(new ZLinkServiceMailbox.Record(
            "owner",
            ZLinkServiceMailbox.Domain.INFRASTRUCTURE,
            List.of(new byte[64]),
            null,
            null,
            null));

        mailbox.close();

        assertEquals(
            0,
            mailbox.pendingMessages(
                ZLinkServiceMailbox.Domain.APPLICATION));
        assertEquals(
            0,
            mailbox.pendingBytes(
                ZLinkServiceMailbox.Domain.APPLICATION));
        assertEquals(
            0,
            mailbox.pendingMessages(
                ZLinkServiceMailbox.Domain.INFRASTRUCTURE));
        assertFalse(mailbox.tryEnqueue(new ZLinkServiceMailbox.Record(
            "owner",
            ZLinkServiceMailbox.Domain.APPLICATION,
            List.of(new byte[1]),
            null,
            null,
            null)));
    }
}
