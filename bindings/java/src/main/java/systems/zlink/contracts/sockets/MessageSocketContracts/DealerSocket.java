/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.messaging.RequestOperation;
import systems.zlink.contracts.messaging.SendOperation;

/** A socket that load-balances sends across its connected peers and can issue routed requests. */
public interface DealerSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setRoutingId(RoutingId rid);
    RoutingId getRoutingId();
    SendOperation send();
    boolean recv(Received result, RecvFlags flags);
    RequestOperation request();
    @Override DealerSocketOptions options();
}
