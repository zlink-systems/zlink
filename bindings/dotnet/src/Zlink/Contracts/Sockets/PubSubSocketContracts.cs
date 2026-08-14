// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Contract for sockets that publish messages under a topic.
/// </summary>
public interface IPublisherSocket : IConnectableSocket
{
    /// <summary>
    ///     Begins publishing under <paramref name="topic" />. The asynchronous
    ///     terminal transfers the parts and waits for local Core admission.
    /// </summary>
    AsyncSendOperation Publish(string topic);

    /// <summary>
    ///     Begins an explicit immediate publish attempt. Add parts and call
    ///     <see cref="SendSubmitOperation.Submit" />; use
    ///     <see cref="SendFlags.DontWait" /> to observe back-pressure without
    ///     waiting.
    /// </summary>
    SendOperation TryPublish(string topic);

    /// <summary>
    ///     Registers a callback invoked when the socket can accept more sends after
    ///     back-pressure. The callback runs on a background dispatch thread.
    /// </summary>
    void OnSendReady(Action handler);
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
    /// <remarks>
    ///     Queue-admission credit returns at dequeue; use
    ///     <see cref="SubscribeRetained(TopicMessage, RecvFlags)" /> only when the
    ///     result lifetime must retain that credit.
    /// </remarks>
    bool Subscribe(TopicMessage result, RecvFlags flags = RecvFlags.None);

    /// <summary>
    ///     Receives the next matching topic message while retaining its
    ///     queue-admission credit with the supplied storage.
    /// </summary>
    /// <param name="result">Reusable storage that owns the retained credit.</param>
    /// <param name="flags">Receive behavior flags.</param>
    /// <returns>
    ///     true on success; false when <see cref="RecvFlags.DontWait" /> is set
    ///     and no message is available.
    /// </returns>
    /// <remarks>
    ///     Starting this receive releases any message and retained credit already
    ///     held by <paramref name="result" />. A successful receive holds its
    ///     credit until the result is disposed or reused. Ordinary
    ///     <see cref="Subscribe(TopicMessage, RecvFlags)" /> returns queue credit
    ///     at dequeue.
    /// </remarks>
    bool SubscribeRetained(TopicMessage result,
        RecvFlags flags = RecvFlags.None);
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
