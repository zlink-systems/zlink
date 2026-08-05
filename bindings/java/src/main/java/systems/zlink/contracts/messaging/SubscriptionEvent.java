/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.internal.ContractAccess;
import java.util.Optional;

/** Canonical XPUB subscription event snapshot. */
public final class SubscriptionEvent {
    private RoutingId routingId;
    private String topic;
    private boolean subscribed;

    static {
        ContractAccess.register(new ContractAccess.SubscriptionEventAccess() {
            @Override
            public SubscriptionEvent create(Optional<RoutingId> routingId,
                                            String topic,
                                            boolean subscribed) {
                return new SubscriptionEvent(routingId, topic, subscribed);
            }

            @Override
            public void adoptFrom(SubscriptionEvent target,
                                  SubscriptionEvent source) {
                target.adoptFrom(source);
            }
        });
    }

    public SubscriptionEvent() {
        this(Optional.empty(), "", false);
    }

    SubscriptionEvent(Optional<RoutingId> routingId, String topic,
                      boolean subscribed) {
        this.routingId = routingId == null ? null : routingId.orElse(null);
        this.topic = topic == null ? "" : topic;
        this.subscribed = subscribed;
    }

    void adoptFrom(SubscriptionEvent source) {
        this.routingId = source.routingId;
        this.topic = source.topic;
        this.subscribed = source.subscribed;
    }

    public Optional<RoutingId> getRoutingId() {
        return Optional.ofNullable(routingId);
    }

    public String topic() {
        return topic;
    }

    public boolean subscribed() {
        return subscribed;
    }
}
