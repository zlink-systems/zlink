/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.eventing.Poller;

final class PollerCloseBusyContractTest {
    @Test
    void busyDestroyLeavesPollerOpenForExplicitRetry() throws Exception {
        TestSupport.assumeNative();
        CompletionNativeFixture.runProbe(BusyProbe.class);
    }

    public static final class BusyProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            Poller poller = Zlink.createPoller();
            core.pollerDestroyBusy.set(1);
            ZlinkCloseException busy = assertThrows(ZlinkCloseException.class, poller::close);
            assertEquals(CloseResult.BUSY, busy.getResult());
            assertEquals(0, poller.size());
            poller.close();
            assertThrows(IllegalStateException.class, poller::size);
            System.exit(0);
        }
    }
}
