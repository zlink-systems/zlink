using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace Bingo.Server.Session.Sessions;

internal sealed class BingoSession(
    IZLinkSessionContext context) : IZLinkSession
{
    public IZLinkSessionContext Context { get; } = context;

    public ValueTask OnConnectedAsync(CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDisconnectedAsync(CancellationToken cancellationToken)
    {
        foreach (var actor in Context.Actors.Bound) await actor.NotifyDisconnectedAsync(cancellationToken);
    }

    public ValueTask OnErrorAsync(
        ZLinkStreamError error,
        CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
            return;

        var actor = RequireSingleBoundActor($"relaying packet '{dispatch.PacketName}'");
        await actor.RelayAsync(
                payload,
                cancellationToken)
            ;
    }

    private IZLinkSessionActor RequireSingleBoundActor(string action)
    {
        var actors = Context.Actors.Bound;
        return actors.Count switch
        {
            1 => actors.Single(),
            0 => throw new InvalidOperationException($"Client must authenticate before {action}."),
            _ => throw new InvalidOperationException($"Exactly one actor must be bound before {action}.")
        };
    }
}
