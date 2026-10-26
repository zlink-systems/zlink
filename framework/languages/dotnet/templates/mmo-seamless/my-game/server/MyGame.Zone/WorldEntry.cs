using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Zone;

public sealed class WorldEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public void Configure()
    {
        Context.Handlers.AddActorPacket<WorldEntryPlayerAppearedNotification, PlayerActor>(nameof(PlayerAppeared));
        Context.Handlers.AddActorPacket<WorldEntryPlayerMovedNotification, PlayerActor>(nameof(PlayerMoved));
        Context.Handlers.AddActorPacket<WorldEntryPlayerLeftNotification, PlayerActor>(nameof(PlayerLeft));
    }
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

[ZLinkSpotActorSendHandler(nameof(PlayerAppeared))]
public sealed class WorldEntryPlayerAppearedNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, PlayerAppeared>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        PlayerAppeared message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(PlayerMoved))]
public sealed class WorldEntryPlayerMovedNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, PlayerMoved>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        PlayerMoved message, CancellationToken ct) => actor.DeliverAsync(message);
}

[ZLinkSpotActorSendHandler(nameof(PlayerLeft))]
public sealed class WorldEntryPlayerLeftNotification : IZLinkEntrySpotActorSendHandler<WorldEntry, PlayerActor, PlayerLeft>
{
    public ValueTask HandleAsync(WorldEntry spot, PlayerActor actor, IZLinkMessageContext context,
        PlayerLeft message, CancellationToken ct) => actor.DeliverAsync(message);
}
