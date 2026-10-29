/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import java.util.Objects;

/** Core diagnostics; disconnectReason holds an unsigned 32-bit value. */
public record ConnectionFailure(
        ConnectionFailureKind kind,
        ConnectionFailureStage stage,
        int normalizedErrno,
        long disconnectReason) {
    public ConnectionFailure {
        Objects.requireNonNull(kind, "kind");
        Objects.requireNonNull(stage, "stage");
    }
}
