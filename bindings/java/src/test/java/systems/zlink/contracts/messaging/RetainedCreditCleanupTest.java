/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class RetainedCreditCleanupTest {
    @Test
    void replaceTransferAndRepeatedReleaseRemainExactlyOnce() {
        AtomicInteger releases = new AtomicInteger();
        RetainedCreditCleanup source = new RetainedCreditCleanup();
        RetainedCreditCleanup target = new RetainedCreditCleanup();

        target.replace(releases::incrementAndGet);
        source.replace(releases::incrementAndGet);
        target.transferFrom(source);
        assertEquals(1, releases.get());

        source.release();
        target.release();
        target.run();
        assertEquals(2, releases.get());
    }
}
