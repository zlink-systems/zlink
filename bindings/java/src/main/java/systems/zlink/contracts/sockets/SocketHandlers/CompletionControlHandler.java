/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

/** Receives an opaque multipart record from a peer's Completion connection. */
@FunctionalInterface
public interface CompletionControlHandler {
    /** The callback owns every message in {@code parts} and must close it once. */
    void onControl(RoutingId sourceRoutingId, List<Message> parts);
}
