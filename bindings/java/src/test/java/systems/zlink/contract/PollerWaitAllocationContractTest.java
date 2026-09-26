/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.contract;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.lang.management.ManagementFactory;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;

final class PollerWaitAllocationContractTest {
    @Test
    void repeatedEmptyWaitReusesNativeOutputStorage() {
        TestSupport.assumeNative();
        var platformBean = ManagementFactory.getThreadMXBean();
        assumeTrue(platformBean instanceof com.sun.management.ThreadMXBean);
        var allocationBean = (com.sun.management.ThreadMXBean) platformBean;
        assumeTrue(allocationBean.isThreadAllocatedMemorySupported());
        allocationBean.setThreadAllocatedMemoryEnabled(true);

        try (Poller poller = Zlink.createPoller()) {
            PollEvents events = new PollEvents(1);
            for (int i = 0; i < 10_000; i++)
                assertEquals(0, poller.wait(events, Duration.ZERO));

            long threadId = Thread.currentThread().threadId();
            long before = allocationBean.getThreadAllocatedBytes(threadId);
            for (int i = 0; i < 20_000; i++)
                poller.wait(events, Duration.ZERO);
            long allocated = allocationBean.getThreadAllocatedBytes(threadId) - before;
            System.out.println("poller wait bytes per call: " + allocated / 20_000);
            assertTrue(allocated / 20_000 < 64,
                "poller wait allocated " + allocated / 20_000 + " bytes per call");
        }
    }
}
