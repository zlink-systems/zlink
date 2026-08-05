using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace ObservabilityOps.Server.Session.Sessions;

internal sealed class ObservabilitySession(IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken) => ValueTask.CompletedTask;

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        foreach (var actor in Context.Actors.Bound) await actor.NotifyDisconnectedAsync(cancellationToken);
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnDispatchAsync(ZLinkSessionDispatchContext dispatch, ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken)) return;
        var actor = Context.Actors.Bound.Count == 1
            ? Context.Actors.Bound.Single()
            : throw new InvalidOperationException("Authenticate exactly one actor before sending an action.");
        await actor.RelayAsync(payload, cancellationToken);
    }
}
