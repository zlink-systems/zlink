/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.TestSupport;
import systems.zlink.contracts.messaging.Message;

final class RequestDefaultTimeoutContractTest {
    @Test
    void omittedTimeoutDelegatesToCoreSocketOption() {
        TestSupport.assumeNative();
        AtomicReference<Duration> asyncTimeout = new AtomicReference<>();
        AtomicReference<Duration> syncTimeout = new AtomicReference<>();
        try (Message asyncMessage = Message.from("async");
             Message syncMessage = Message.from("sync")) {
            MessageOperations.request((parts, timeout) -> {
                asyncTimeout.set(timeout);
                return null;
            }, (parts, timeout) -> List.of())
                .message(asyncMessage).submit();
            MessageOperations.request((parts, timeout) -> null,
                (parts, timeout) -> {
                    syncTimeout.set(timeout);
                    return List.of();
                }).message(syncMessage).submit_sync();
        }
        assertEquals(Duration.ZERO, asyncTimeout.get());
        assertEquals(Duration.ZERO, syncTimeout.get());
    }
}
