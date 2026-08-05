// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     One active subscription: a topic filter and whether it is a pattern.
/// </summary>
/// <param name="Filter">The subscription topic or pattern.</param>
/// <param name="IsPattern">Whether <paramref name="Filter" /> is a pattern rather than an exact topic.</param>
public sealed record SubscriptionEntry(string Filter, bool IsPattern);

/// <summary>
///     A subscriber's subscribe or unsubscribe as observed by an XPUB socket.
/// </summary>
public sealed partial class SubscriptionEvent
{
    /// <summary>
    ///     Creates an empty event for reuse across receives.
    /// </summary>
    public SubscriptionEvent()
    {
    }

    /// <summary>
    ///     Gets the routing id of the subscriber, when known.
    /// </summary>
    public RoutingId? RoutingId { get; private set; }

    /// <summary>
    ///     Gets the topic that was subscribed or unsubscribed.
    /// </summary>
    public string Topic { get; private set; } = string.Empty;

    /// <summary>
    ///     Gets whether this event subscribed or unsubscribed the topic.
    /// </summary>
    public bool Subscribed { get; private set; }
}