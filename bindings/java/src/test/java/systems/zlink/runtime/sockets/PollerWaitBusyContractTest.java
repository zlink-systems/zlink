/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ConfigResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkConfigException;
import systems.zlink.contracts.eventing.PollEvents;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.runtime.nativeapi.NativeErrno;

final class PollerWaitBusyContractTest {
    @Test
    void coreOwnsConcurrentWaitAndClearBusyResults() throws Exception {
        TestSupport.assumeNative();
        CompletionNativeFixture.runProbe(BusyProbe.class);
    }

    public static final class BusyProbe {
        public static void main(String[] args) throws Throwable {
            CompletionNativeFixture core = new CompletionNativeFixture();
            core.blockPollerWait = true;
            Poller poller = Zlink.createPoller();
            CompletableFuture<Integer> active = CompletableFuture.supplyAsync(
                () -> poller.wait(new PollEvents(1), Duration.ofSeconds(5)));
            try {
                assertTrue(core.pollerWaitEntered.await(
                    TestSupport.DEFAULT_TIMEOUT_MS, TimeUnit.MILLISECONDS));
                ZlinkConfigException waitBusy = assertThrows(
                    ZlinkConfigException.class,
                    () -> poller.wait(new PollEvents(1), Duration.ZERO));
                assertEquals(ConfigResult.BUSY, waitBusy.getResult());
                assertEquals(NativeErrno.EBUSY, waitBusy.getNativeErrno());
                assertEquals(2, core.pollerWaitCalls.get());

                core.pollerDestroyBusy.set(1);
                ZlinkCloseException clearBusy = assertThrows(
                    ZlinkCloseException.class, poller::clear);
                assertEquals(CloseResult.BUSY, clearBusy.getResult());
            } finally {
                core.releasePollerWait.countDown();
            }
            assertEquals(0, active.get(TestSupport.DEFAULT_TIMEOUT_MS,
                TimeUnit.MILLISECONDS));
            poller.close();
            System.exit(0);
        }
    }
}
