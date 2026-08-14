/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.util.Objects;
import java.util.function.Supplier;

/** Serializes one socket's short, complete outbound record attempts. */
final class OutboundRecordAttemptGate {
    private final Object lock = new Object();

    <T> T call(Supplier<T> attempt) {
        Objects.requireNonNull(attempt, "attempt");
        synchronized (lock) {
            return attempt.get();
        }
    }

    void run(Runnable attempt) {
        Objects.requireNonNull(attempt, "attempt");
        synchronized (lock) {
            attempt.run();
        }
    }
}
