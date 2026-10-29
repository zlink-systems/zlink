/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.TopicMessage;

import java.util.List;
import java.util.Optional;

/** A receive-only subscriber filtered by its subscriptions. */
public interface SubSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);

    /** Copies the current Core connection-attempt facts and acknowledges their readiness. */
    List<ConnectionAttempt> connectionAttemptsSnapshot();

    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setSubscription(String filter);
    void unsetSubscription(String filter);
    Optional<SubscriptionEntry> subscriptionAt(int index);
    boolean subscribe(TopicMessage result, RecvFlags flags);
    @Override SubSocketOptions options();
}
