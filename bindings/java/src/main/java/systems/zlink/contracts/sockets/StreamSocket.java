/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;

/** Exchanges framed packets with raw TCP peers. */
public interface StreamSocket extends Socket {
    void bind(String endpoint);
    void unbind(String endpoint);
    /** Disconnects the peer identified by its STREAM routing id. */
    void disconnectRid(RoutingId rid);
    void setRoutingId(RoutingId rid);
    RoutingId getRoutingId();
    SendOperation send(RoutingId rid);
    boolean recv(Received result, RecvFlags flags);
    void onPacket(StreamPacketHandler handler);
    @Override StreamSocketOptions options();
}
