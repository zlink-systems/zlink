/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;

/** An exclusive one-to-one peering socket with no routing. */
public interface PairSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    SendOperation send();
    boolean recv(Received result, RecvFlags flags);
    /**
     * Receives for a Framework backend and retains the origin Core HWM credit
     * until {@code result} is closed or reused.
     */
    boolean recvRetained(Received result, RecvFlags flags);
}
