// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Contract for sockets that publish messages under a topic.
/// </summary>
public interface IPublisherSocket : IConnectableSocket
{
    /// <summary>
    ///     Begins publishing under <paramref name="topic" />. Publish is
    ///     synchronous: PUB semantics are lossy, so the publisher never waits at
    ///     the high-water mark and the terminal
    ///     <see cref="PublishSubmitOperation.Submit" /> completes on the calling
    ///     thread. With <c>NODROP</c> a full subscriber surfaces immediately as
    ///     <see cref="ZlinkSubmitException" />; the retry policy belongs to the
    ///     application.
    /// </summary>
    PublishOperation Publish(string topic);

    /// <summary>
    ///     Begins an explicit immediate publish attempt. Add parts and call
    ///     <see cref="SendSubmitOperation.Submit" />; use
    ///     <see cref="SendFlags.DontWait" /> to observe back-pressure as a
    ///     <c>false</c> result rather than an exception.
    /// </summary>
    TryPublishOperation TryPublish(string topic);
}

/// <summary>
///     Contract for sockets that subscribe to published topics.
/// </summary>
public interface ISubscriberSocket : IConnectableSocket
{
    /// <summary>
    ///     Adds a subscription for <paramref name="topicOrPattern" />. Subscriptions
    ///     accumulate; a socket may hold several at once.
    /// </summary>
    void SetSubscription(string topicOrPattern);

    /// <summary>
    ///     Removes a subscription previously added for
    ///     <paramref name="topicOrPattern" />.
    /// </summary>
    void UnsetSubscription(string topicOrPattern);

    /// <summary>
    ///     Gets the active subscription at <paramref name="index" />, or null when
    ///     the index is out of range.
    /// </summary>
    SubscriptionEntry? SubscriptionAt(int index);

    /// <summary>
    ///     Receives the next matching topic message into caller-provided storage.
    /// </summary>
    /// <param name="result">Reusable storage overwritten on success.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set and
    ///     no message is available.
    /// </returns>
    bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None);
}

/// <summary>
///     Contract for a PUB socket: topic-filtered publish that drops messages with
///     no matching subscriber.
/// </summary>
public interface IPubSocket : IPublisherSocket
{
    /// <summary>
    ///     Gets the PUB-specific typed options facade.
    /// </summary>
    new PubSocketOptions Options { get; }

    /// <summary>
    ///     Sets the routing id used to identify this publisher socket. Apply before
    ///     binding or connecting so peers observe the configured identity.
    /// </summary>
    void SetRoutingId(RoutingId routingId);

    /// <summary>
    ///     Gets the routing id currently assigned to this publisher socket.
    /// </summary>
    RoutingId GetRoutingId();

}

/// <summary>
///     Contract for a SUB socket: receive-only subscriber filtered by its
///     subscriptions.
/// </summary>
public interface ISubSocket : ISubscriberSocket
{
    /// <summary>
    ///     Gets the SUB-specific typed options facade.
    /// </summary>
    new SubSocketOptions Options { get; }

    /// <summary>
    ///     Sets the routing id used to identify this subscriber socket. Apply before
    ///     binding or connecting so peers observe the configured identity.
    /// </summary>
    void SetRoutingId(RoutingId routingId);

    /// <summary>
    ///     Gets the routing id currently assigned to this subscriber socket.
    /// </summary>
    RoutingId GetRoutingId();

}

/// <summary>
///     Contract for an XPUB socket: like PUB, but also surfaces subscriber
///     subscription and unsubscription events via
///     <see cref="ReceiveSubscriptionEvent" />.
/// </summary>
public interface IXPubSocket : IPublisherSocket
{
    /// <summary>
    ///     Gets the PUB-specific typed options facade.
    /// </summary>
    new PubSocketOptions Options { get; }

    /// <summary>
    ///     Receives the next subscriber (un)subscription event into
    ///     caller-provided storage.
    /// </summary>
    /// <param name="result">Reusable storage overwritten on success.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set and
    ///     no event is available.
    /// </returns>
    bool ReceiveSubscriptionEvent(SubscriptionEvent result,
        RecvFlags flags = RecvFlags.None);
}

/// <summary>
///     Contract for an XSUB socket: a subscriber whose subscriptions are carried as
///     messages rather than set as socket options.
/// </summary>
public interface IXSubSocket : ISubscriberSocket
{
    /// <summary>
    ///     Gets the SUB-specific typed options facade.
    /// </summary>
    new SubSocketOptions Options { get; }
}
