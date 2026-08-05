namespace Zlink.Framework.Contracts.Streams;

public interface IZLinkSession
{
    IZLinkSessionContext Context { get; }

    void Configure()
    {
    }

    ValueTask OnConnectedAsync(
        CancellationToken cancellationToken);

    ValueTask OnDisconnectedAsync(
        CancellationToken cancellationToken);

    ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken);

    /// <summary>
    ///     Handles a framework-owned inbound stream payload.
    /// </summary>
    /// <remarks>
    ///     The payload is a framework <see cref="ZLinkMessage" />. Session code may
    ///     decode it or pass it to framework APIs such as
    ///     <see cref="IZLinkSessionActor.RelayAsync" />.
    /// </remarks>
    ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        _ = dispatch;
        _ = payload;
        _ = cancellationToken;
        return ValueTask.CompletedTask;
    }
}

public interface IZLinkSessionClient
{
    IZLinkSessionSendCall Send<TMessage>(TMessage message);

    IZLinkSessionReplyCall Reply<TMessage>(TMessage message);
}

public interface IZLinkSessionActors
{
    IReadOnlyCollection<IZLinkSessionActor> Bound { get; }

    ValueTask<IZLinkSessionActor> BindAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);

    ValueTask<IZLinkSessionActor> BindOrGetAsync(
        ActorRef actor,
        CancellationToken cancellationToken = default);

    IZLinkSessionActor? Find(string actorId);
}

public interface IZLinkSessionContext
{
    string SessionId { get; }

    RoutingId? RoutingId { get; }

    string? LocalAddr { get; }

    string? RemoteAddr { get; }

    IZLinkSessionClient Client { get; }

    IZLinkSessionActors Actors { get; }

    IZLinkSessionHandlerRegistry Handlers { get; }

    ValueTask CloseAsync();
}

public interface IZLinkSessionSendCall
    : IZLinkMetadataCall<IZLinkSessionSendCall>
{
    IZLinkSessionSendCall Compress();

    /// <summary>
    /// Submits the session send. Input validation and local transport
    /// acceptance complete before this method returns. If the transport queue
    /// is full, the call waits up to the configured send timeout.
    /// </summary>
    ValueTask Async(
        CancellationToken cancellationToken = default);
}

public interface IZLinkSessionReplyCall
{
    IZLinkSessionReplyCall Compress();

    /// <summary>
    /// Submits a reply for the currently handled request packet. The reply uses
    /// the request packet name and does not expose PacketName because a reply
    /// must keep the request correlation.
    /// </summary>
    ValueTask Async(
        CancellationToken cancellationToken = default);
}
