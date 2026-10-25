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
    public async ValueTask HandleAsync(
        WorldEntry entry,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        var location = ZonePositionDomain.ResolveLocation(request.X);
        if (location.Rejection is not null)
        {
            await actor.Context.BoundSession.Send(location.Rejection).Async();
            return;
        }
        actor.Move(location.ZoneId!, new EnterZone(new Position(request.X), 0, 0));
    }
}
