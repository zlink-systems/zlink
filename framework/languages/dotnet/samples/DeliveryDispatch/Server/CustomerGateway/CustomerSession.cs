using Zlink.Framework.Contracts.Streams;

namespace DeliveryDispatch.Server.CustomerGateway;

internal sealed class CustomerSession(
    IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        foreach (var actor in Context.Actors.Bound)
        {
            await actor.NotifyDisconnectedAsync(cancellationToken);
        }
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
        {
            return;
        }

        var actor = Context.Actors.Bound.Single();
        await actor.RelayAsync(payload, cancellationToken);
    }
}
