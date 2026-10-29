/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import java.util.Objects;

/**
 * One connect registration observed by Core. The unsigned 64-bit intentId, revision and
 * lastFailureId are opaque equality tokens, including negative Java long values. Core supplies
 * nonzero intentId and revision values. A zero lastFailureId is allowed and means no failure has
 * been recorded.
 */
public record ConnectionAttempt(
        String endpoint,
        long intentId,
        long revision,
        long lastFailureId,
        ConnectionAttemptState state,
        ConnectionFailure lastFailure) {
    public ConnectionAttempt {
        Objects.requireNonNull(endpoint, "endpoint");
        Objects.requireNonNull(state, "state");
        Objects.requireNonNull(lastFailure, "lastFailure");
    }
}
