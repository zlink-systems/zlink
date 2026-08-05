using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Streams;

namespace GameQuest.GameApi.Session;

internal sealed class GameQuestSession(
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
            await actor.NotifyDisconnectedAsync(cancellationToken);
    }

    public ValueTask OnErrorAsync(ZLinkStreamError error, CancellationToken cancellationToken)
    {
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnDispatchAsync(
        ZLinkSessionDispatchContext dispatch,
        ZLinkMessage payload,
        CancellationToken cancellationToken)
    {
        if (!await Context.Handlers.TryHandleAsync(dispatch, payload, cancellationToken))
        {
            var actor = Context.Actors.Bound.Count switch
            {
                1 => Context.Actors.Bound.Single(),
                0 => throw new InvalidOperationException(
                    $"Client must join a player session before sending '{dispatch.PacketName}'."),
                _ => throw new InvalidOperationException("A GameQuest session may bind exactly one player actor.")
            };
            await actor.RelayAsync(payload, cancellationToken);
        }
    }
}
