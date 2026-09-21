/* SPDX-License-Identifier: Apache-2.0 */
package systems.zlink.httpclient.internal;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;

final class HttpClientTextTest {
    @Test
    void normalizesSubMillisecondTimeoutToOneMillisecond() {
        assertEquals(Duration.ofMillis(1), HttpClientText.normalizeTimeout(Duration.ofNanos(1)));
    }

    @Test
    void rejectsTimeoutOutsideTheContractRange() {
        assertThrows(
                ZLinkFrameworkException.class,
                () ->
                        HttpClientText.normalizeTimeout(
                                Duration.ofMillis(Integer.MAX_VALUE).plusMillis(1)));
    }
}
