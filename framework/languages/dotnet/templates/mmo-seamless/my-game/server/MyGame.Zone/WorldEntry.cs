using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Zone;

public sealed class WorldEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;
}

[ZLinkSpotActorSendHandler(nameof(JoinWorldRequest))]
public sealed class EnterWorldHandler
    : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, JoinWorldRequest>
{
    public ValueTask HandleAsync(
        WorldEntry entry,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        actor.Move(new EnterZone(request.X, 0));
        return ValueTask.CompletedTask;
    }
}
