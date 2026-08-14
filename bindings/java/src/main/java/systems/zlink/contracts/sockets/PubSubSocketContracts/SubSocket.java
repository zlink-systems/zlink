/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.SubscriptionEntry;
import systems.zlink.contracts.messaging.TopicMessage;
import java.util.Optional;

/** A receive-only subscriber filtered by its subscriptions. */
public interface SubSocket extends Socket {
    void bind(String endpoint);
    void connect(String endpoint);
    void unbind(String endpoint);
    void disconnect(String endpoint);
    void disconnectRid(RoutingId routingId);
    void setSubscription(String filter);
    void unsetSubscription(String filter);
    Optional<SubscriptionEntry> subscriptionAt(int index);
    boolean subscribe(TopicMessage result, RecvFlags flags);
    /**
     * Subscribes for a Framework backend and retains each payload part's
     * origin Core HWM credit until {@code result} is closed or reused.
     */
    boolean subscribeRetained(TopicMessage result, RecvFlags flags);
    @Override SubSocketOptions options();
}
