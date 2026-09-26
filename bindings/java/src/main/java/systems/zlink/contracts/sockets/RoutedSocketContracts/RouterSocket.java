/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import java.util.List;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.SendOperation;

/** Routes messages to peers addressed by routing id; the request/reply server side. */
public interface RouterSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setRoutingId(RoutingId rid);
    RoutingId getRoutingId();

    /**
     * Returns every selected route atomically, one row per routing id. A
     * successful call clears {@code POLLROUTE} readiness unless a later change
     * raced with it. Keep one route observer per socket.
     */
    List<RouterRoute> routesSnapshot();
    SendOperation send(RoutingId rid);
    boolean recv(Received result, RecvFlags flags);
    RequestOperation request(RoutingId rid);
    ReplyOperation reply(RoutingId rid, ReplyToken token);
    @Override RouterSocketOptions options();
}
