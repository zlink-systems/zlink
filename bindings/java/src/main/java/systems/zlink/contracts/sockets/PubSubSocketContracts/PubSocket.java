/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SendOperation;

/** A topic-filtered publisher that drops messages with no matching subscriber. */
public interface PubSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setRoutingId(RoutingId rid);
    SendOperation publish(String topicId);
    @Override PubSocketOptions options();
}
