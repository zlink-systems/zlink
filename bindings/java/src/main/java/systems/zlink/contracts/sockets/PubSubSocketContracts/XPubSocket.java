/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.PublishOperation;
import systems.zlink.contracts.messaging.SubscriptionEvent;

import java.util.List;

/** A publisher that also surfaces subscriber subscription events. */
public interface XPubSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);

    /** Copies the current Core connection-attempt facts and acknowledges their readiness. */
    List<ConnectionAttempt> connectionAttemptsSnapshot();

    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setRoutingId(RoutingId rid);
    PublishOperation publish(String topicId);
    boolean receiveSubscriptionEvent(SubscriptionEvent result, RecvFlags flags);
    @Override PubSocketOptions options();
}
