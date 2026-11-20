/* SPDX-License-Identifier: MPL-2.0 */
package systems.zlink.runtime.sockets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.TestSupport;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.eventing.ZlinkTimer;

import java.time.Duration;

public class TimerCloseContractTest {
    @Test
    public void failedTimerCloseCanBeRetriedAfterPollerRemoval() {
        TestSupport.assumeNative();

        try (ZlinkTimer timer = Zlink.createTimer();
                Poller poller = Zlink.createPoller()) {
            poller.add(timer, 1);

            ZlinkCloseException failure = assertThrows(ZlinkCloseException.class, timer::close);
            assertEquals(CloseResult.BUSY, failure.getResult());
            assertTrue(poller.remove(timer));

            timer.close();
            assertThrows(IllegalStateException.class, () -> timer.start(Duration.ofMillis(1), 1));
        }
    }
}
