/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RoutedSendOperation;
import systems.zlink.contracts.messaging.SendOperation;

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
    /**
     * Builds an asynchronous send to the peer identified by {@code rid}.
     * The returned stage completes when Core accepts the complete record.
     */
    RoutedSendOperation sendAsync(RoutingId rid);
    boolean recv(Received result, RecvFlags flags);
    void onPacket(StreamPacketHandler handler);
    @Override StreamSocketOptions options();
}
