package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.ArrayList;
import java.util.List;
import org.junit.jupiter.api.Test;

final class ZLinkServiceMailboxSchedulerTest {
    @Test
    void enforcesOwnerBudgetAndRestoresLevelReadyAfterPartialDrain() {
        ZLinkServiceMailboxScheduler scheduler = new ZLinkServiceMailboxScheduler(2, 8);
        ZLinkServiceMailboxScheduler.Owner owner =
            new ZLinkServiceMailboxScheduler.Owner("spot", "alpha", 3);
        List<Integer> completed = new ArrayList<>();

        assertEquals(ZLinkServiceMailboxScheduler.Admission.ACCEPTED,
            scheduler.admit(owner, ZLinkServiceMailboxScheduler.Domain.APPLICATION,
                new ZLinkServiceMailboxScheduler.Work(4, () -> completed.add(1))));
        assertEquals(ZLinkServiceMailboxScheduler.Admission.ACCEPTED,
            scheduler.admit(owner, ZLinkServiceMailboxScheduler.Domain.APPLICATION,
                new ZLinkServiceMailboxScheduler.Work(4, () -> completed.add(2))));
        assertEquals(ZLinkServiceMailboxScheduler.Admission.BACKPRESSURED,
            scheduler.admit(owner, ZLinkServiceMailboxScheduler.Domain.APPLICATION,
                new ZLinkServiceMailboxScheduler.Work(1, () -> completed.add(3))));

        assertEquals(owner,
            scheduler.pollReady(ZLinkServiceMailboxScheduler.Domain.APPLICATION));
        assertEquals(1, scheduler.drain(
            owner, ZLinkServiceMailboxScheduler.Domain.APPLICATION, 1, work -> work.action().run()));
        assertEquals(owner,
            scheduler.pollReady(ZLinkServiceMailboxScheduler.Domain.APPLICATION));
        assertEquals(1, scheduler.drain(
            owner, ZLinkServiceMailboxScheduler.Domain.APPLICATION, 2, work -> work.action().run()));
        assertEquals(List.of(1, 2), completed);
        assertEquals(0, scheduler.pendingMessages());
        assertEquals(0, scheduler.pendingBytes());
    }

    @Test
    void sealRejectsNewAdmissionWithoutDroppingQueuedWork() {
        ZLinkServiceMailboxScheduler scheduler = new ZLinkServiceMailboxScheduler(2, 8);
        ZLinkServiceMailboxScheduler.Owner owner =
            new ZLinkServiceMailboxScheduler.Owner("node", "beta", 1);
        ZLinkServiceMailboxScheduler.Work work =
            new ZLinkServiceMailboxScheduler.Work(2, () -> { });

        assertEquals(ZLinkServiceMailboxScheduler.Admission.ACCEPTED,
            scheduler.admit(owner, ZLinkServiceMailboxScheduler.Domain.INFRASTRUCTURE, work));
        scheduler.seal();
        assertEquals(ZLinkServiceMailboxScheduler.Admission.SEALED,
            scheduler.admit(owner, ZLinkServiceMailboxScheduler.Domain.INFRASTRUCTURE, work));
        assertEquals(1, scheduler.pendingMessages());
        assertEquals(2, scheduler.pendingBytes());
    }
}
