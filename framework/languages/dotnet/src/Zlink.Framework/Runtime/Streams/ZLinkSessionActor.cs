namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionActor(
    ZLinkSessionContext context,
    string actorId,
    RoutingId sessionRid,
    string bindingToken)
    : IZLinkSessionActor
{
    private readonly object _disconnectGate = new();
    private Task? _disconnectTask;

    internal ZLinkSessionContext Context { get; } = context;

    internal RoutingId SessionRid { get; } = sessionRid;

    internal string BindingToken { get; } = bindingToken;
    public string ActorId { get; } = actorId;

    public ActorRef Ref => Route.Ref;

    internal ZLinkSessionBindingRoute Route
    {
        get
        {
            if (TryGetRoute(out var route))
                return route;
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.InvalidOperation,
                $"Actor '{ActorId}' session binding is stale.",
                ZLinkRetryAdvice.RetryAfterBackoff);
        }
    }

    internal bool TryGetRoute(out ZLinkSessionBindingRoute route) =>
        Context.Runtime.TryGetSessionActorRoute(
            ActorId,
            BindingToken,
            this,
            out route);

    public ValueTask RelayAsync(
        ZLinkMessage payload,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(payload);
        var raw = payload.ToRawMessage(Context.Runtime.Registration.Codecs);
        return Context.RelayActorRefAsync(this, raw, cancellationToken)
            .EnsureAcceptedAsync(
                "Session Actor relay",
                ZLinkFrameworkErrorKind.NotFound);
    }

    public ValueTask NotifyDisconnectedAsync(CancellationToken cancellationToken = default)
    {
        Task notification;
        lock (_disconnectGate)
        {
            _disconnectTask ??= Context
                .NotifyActorRefDisconnectedAsync(this, CancellationToken.None)
                .AsTask();
            notification = _disconnectTask;
        }

        return cancellationToken.CanBeCanceled
            ? new ValueTask(notification.WaitAsync(cancellationToken))
            : new ValueTask(notification);
    }
}
