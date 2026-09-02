/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.messaging.StreamPacket;

/** Exchanges framed packets with raw TCP peers. */
public interface StreamSocket extends Socket {
    void bind(String endpoint);
    void unbind(String endpoint);
    /** Disconnects the peer identified by its STREAM routing id. */
    void disconnectRid(RoutingId rid);
    void setRoutingId(RoutingId rid);
    RoutingId getRoutingId();
    /** Builds a synchronous send to the peer identified by {@code rid}. */
    SendOperation send(RoutingId rid);
    boolean recv(Received result, RecvFlags flags);
    boolean recvPacket(StreamPacket result, RecvFlags flags);
    @Override StreamSocketOptions options();
}
