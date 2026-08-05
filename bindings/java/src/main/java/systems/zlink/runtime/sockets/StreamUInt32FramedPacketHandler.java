/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.messaging.Message;

@FunctionalInterface
interface StreamUInt32FramedPacketHandler {
    void onPacket(int routingId, Message header, Message body);
}
