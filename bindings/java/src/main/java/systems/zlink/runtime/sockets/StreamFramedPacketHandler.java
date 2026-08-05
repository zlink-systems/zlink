/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;

@FunctionalInterface
interface StreamFramedPacketHandler {
    void onPacket(RoutingId routingId, Message header, Message body);
}
