using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace MyGame.Zone;

[ZLinkSpotActorSendHandler(nameof(JoinWorldRequest))]
public sealed class ReconnectHandler
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, JoinWorldRequest>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity does not match the bound Actor.");
        spot.Remember(actor);
        await actor.SendStateAsync();
    }
}

[ZLinkSpotActorSendHandler(nameof(MoveRequest))]
public sealed class MoveHandler : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, MoveRequest>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MoveRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.Sequence != actor.Sequence + 1)
            throw new InvalidDataException("Move sequence must increase by one.");
        var zoneId = GameNames.ZoneId(request.X);
        var position = new EnterZone(request.X, request.Sequence);
        if (zoneId != spot.Context.SpotId)
        {
            actor.Move(position);
            return;
        }
        actor.Apply(position);
        spot.Remember(actor);
        await actor.SendStateAsync();
    }
}

public sealed class BorderTick : IZLinkSpotTimerHandler<ZoneSpot>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken
    ) => spot.PublishBorderAsync(cancellationToken);
}

public sealed class BorderHandler : IZLinkSpotSubscriptionHandler<ZoneSpot, BorderPresence>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        BorderPresence presence,
        Zlink.Framework.Contracts.Channels.ZLinkPublishMessageContext context,
        CancellationToken cancellationToken
    ) => spot.ShowBorderAsync(presence);
}

[ZLinkSpotActorSendHandler(nameof(BorderVisible))]
public sealed class BorderDelivery
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, BorderVisible>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        BorderVisible presence,
        CancellationToken cancellationToken
    ) => spot.DeliverAsync(actor, presence);
}

[ZLinkSpotActorSendHandler(nameof(OpenDungeonRequest))]
public sealed class OpenDungeonHandler(IZLinkSpotManager spots)
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, OpenDungeonRequest>
{
    public async ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        OpenDungeonRequest request,
        CancellationToken cancellationToken
    )
    {
        var dungeon = await spots
            .GetOrCreate(request.DungeonId, GameNames.DungeonType)
            .InMesh(GameNames.WorldMesh)
            .Async(cancellationToken);
        if (dungeon.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var opened = await spot
            .Context.Outbound.RequestToSpot(request.DungeonId, request)
            .Async<DungeonOpened>(cancellationToken);
        await actor.Context.BoundSession.Send(opened).Async();
    }
}
