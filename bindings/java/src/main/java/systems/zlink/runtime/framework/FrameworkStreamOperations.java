/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.framework;

import java.time.Duration;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.StreamSocket;

/** Qualified internal bridge for Framework-owned raw STREAM framing. */
public final class FrameworkStreamOperations {
    private static volatile Access access;

    private FrameworkStreamOperations() {
    }

    public interface Access {
        CompletionStage<Void> send(
            StreamSocket socket,
            RoutingId routingId,
            List<Message> parts,
            Duration timeout);
    }

    public static void register(Access value) {
        access = Objects.requireNonNull(value, "value");
    }

    public static CompletionStage<Void> send(
        StreamSocket socket,
        RoutingId routingId,
        List<Message> parts,
        Duration timeout) {
        Access current = access;
        if (current == null) {
            throw new IllegalStateException(
                "native STREAM runtime access is unavailable");
        }
        return current.send(socket, routingId, parts, timeout);
    }
}
