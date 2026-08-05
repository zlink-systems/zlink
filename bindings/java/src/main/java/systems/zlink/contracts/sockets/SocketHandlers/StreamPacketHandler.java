/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.core.RoutingId;
/** Callback invoked for each incoming stream socket packet. */
@FunctionalInterface
public interface StreamPacketHandler {
    void onPacket(RoutingId routingId, Message header, Message body);
}
