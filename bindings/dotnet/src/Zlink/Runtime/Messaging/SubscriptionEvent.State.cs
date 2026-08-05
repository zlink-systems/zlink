// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

public sealed partial class SubscriptionEvent
{
    internal SubscriptionEvent(RoutingId? routingId, string topic,
        bool subscribed)
    {
        Populate(routingId, topic, subscribed);
    }

    internal void Populate(RoutingId? routingId, string topic, bool subscribed)
    {
        RoutingId = routingId;
        Topic = topic ?? string.Empty;
        Subscribed = subscribed;
    }
}