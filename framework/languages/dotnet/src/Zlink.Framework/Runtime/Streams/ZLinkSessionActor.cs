namespace Zlink.Framework.Runtime.Streams;

internal sealed class ZLinkSessionActor : IZLinkSessionActor
{
    private readonly Lazy<Task> _disconnectTask;

    internal ZLinkSessionContext Context { get; }

    internal RoutingId SessionRid { get; }

    internal string BindingToken { get; }
    public string ActorId { get; }

    internal ZLinkSessionActor(
        ZLinkSessionContext context,
        string actorId,
        RoutingId sessionRid,
        string bindingToken)
    {
        Context = context;
        ActorId = actorId;
        SessionRid = sessionRid;
        BindingToken = bindingToken;
        _disconnectTask = new Lazy<Task>(
            () => Context.NotifyActorRefDisconnectedAsync(this, CancellationToken.None).AsTask(),
            LazyThreadSafetyMode.ExecutionAndPublication);
    }

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
        var notification = _disconnectTask.Value;

        return cancellationToken.CanBeCanceled
            ? new ValueTask(notification.WaitAsync(cancellationToken))
            : new ValueTask(notification);
    }
}
