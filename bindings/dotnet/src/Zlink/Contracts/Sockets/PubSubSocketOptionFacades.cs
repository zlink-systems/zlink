// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

/// <summary>
///     Typed facade over STREAM-specific socket options.
/// </summary>
public sealed partial class StreamSocketOptions : CommonSocketOptions
{
    /// <summary>Gets or sets the STREAM receive mode.</summary>
    public StreamReceiveMode ReceiveMode
    {
        get => (StreamReceiveMode)Socket.GetOption(
            SocketOptions.StreamReceiveMode);
        set
        {
            if (value is not (StreamReceiveMode.Raw
                or StreamReceiveMode.Packet))
                throw new ArgumentOutOfRangeException(nameof(value));
            Socket.SetOption(SocketOptions.StreamReceiveMode, (int)value);
        }
    }

    /// <summary>
    ///     Gets or sets whether peer connect and disconnect events are delivered to
    ///     the application as messages.
    /// </summary>
    public bool Notify
    {
        get => Socket.GetOption(SocketOptions.StreamNotify) != 0;
        set => Socket.SetOption(SocketOptions.StreamNotify, value ? 1 : 0);
    }
}

/// <summary>
///     Typed facade over PUB/XPUB-specific socket options.
/// </summary>
public sealed partial class PubSocketOptions : CommonSocketOptions
{
    /// <summary>
    ///     Gets or sets whether every subscription message is delivered to the
    ///     application, including duplicates, rather than only the first per topic.
    /// </summary>
    public bool Verbose
    {
        get => Socket.GetOption(SocketOptions.XPubVerbose) != 0;
        set => Socket.SetOption(SocketOptions.XPubVerbose, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether every subscription and unsubscription message is
    ///     delivered, including duplicates.
    /// </summary>
    public bool Verboser
    {
        get => Socket.GetOption(SocketOptions.XPubVerboser) != 0;
        set => Socket.SetOption(SocketOptions.XPubVerboser, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether subscriptions are handled manually via
    ///     <see cref="ApproveSubscribe" /> and <see cref="RejectSubscribe" /> instead
    ///     of being accepted automatically.
    /// </summary>
    public bool Manual
    {
        get => Socket.GetOption(SocketOptions.XPubManual) != 0;
        set => Socket.SetOption(SocketOptions.XPubManual, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets manual subscription handling that also replays the last
    ///     cached message per topic to a newly accepted subscriber.
    /// </summary>
    public bool ManualLastValue
    {
        get => Socket.GetOption(SocketOptions.XPubManualLastValue) != 0;
        set => Socket.SetOption(SocketOptions.XPubManualLastValue,
            value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets whether a send blocked by back-pressure raises an error
    ///     instead of silently dropping the message.
    /// </summary>
    public bool NoDrop
    {
        get => Socket.GetOption(SocketOptions.XPubNoDrop) != 0;
        set => Socket.SetOption(SocketOptions.XPubNoDrop, value ? 1 : 0);
    }

    /// <summary>
    ///     Gets or sets a message automatically sent to each newly connected
    ///     subscriber. The getter returns a new message owned by the caller.
    /// </summary>
    public Message WelcomeMessage
    {
        get => Message.From(Socket.GetOption(SocketOptions.XPubWelcomeMsg));
        set
        {
            if (value == null)
                throw new ArgumentNullException(nameof(value));
            Socket.SetOption(SocketOptions.XPubWelcomeMsg,
                value.AsReadOnlySpan());
        }
    }

    /// <summary>
    ///     Gets the number of distinct topics currently subscribed across connected
    ///     subscribers.
    /// </summary>
    public int TopicsCount => Socket.GetOption(SocketOptions.TopicsCount);

    /// <summary>
    ///     Accepts a pending subscription from <paramref name="routingId" />.
    ///     Requires <see cref="Manual" /> mode.
    /// </summary>
    public void ApproveSubscribe(RoutingId routingId)
    {
        Socket.SetOption(SocketOptions.XPubApproveSubscribe, routingId.ToHex());
    }

    /// <summary>
    ///     Declines a pending subscription from <paramref name="routingId" />.
    ///     Requires <see cref="Manual" /> mode.
    /// </summary>
    public void RejectSubscribe(RoutingId routingId)
    {
        Socket.SetOption(SocketOptions.XPubRejectSubscribe, routingId.ToHex());
    }
}

/// <summary>
///     Typed facade over SUB-specific socket options.
/// </summary>
public sealed partial class SubSocketOptions : CommonSocketOptions
{
    /// <summary>
    ///     Gets the number of active subscriptions on this socket.
    /// </summary>
    public int TopicsCount => Socket.GetOption(SocketOptions.SubTopicsCount);
}
