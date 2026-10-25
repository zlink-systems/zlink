using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

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
        await spot.ReconnectAsync(actor, cancellationToken);
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
        await spot.MoveAsync(actor, request, cancellationToken);
    }
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

[ZLinkSpotActorSendHandler(nameof(PlayerAppeared))]
public sealed class PlayerAppearedDelivery
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, PlayerAppeared>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        PlayerAppeared presence,
        CancellationToken cancellationToken
    ) => actor.Context.BoundSession.Send(presence).Async();
}

[ZLinkSpotActorSendHandler(nameof(PlayerMoved))]
public sealed class PlayerMovedDelivery
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, PlayerMoved>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        PlayerMoved presence,
        CancellationToken cancellationToken
    ) => actor.Context.BoundSession.Send(presence).Async();
}

[ZLinkSpotActorSendHandler(nameof(PlayerLeft))]
public sealed class PlayerLeftDelivery
    : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, PlayerLeft>
{
    public ValueTask HandleAsync(
        ZoneSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        PlayerLeft presence,
        CancellationToken cancellationToken
    ) => actor.Context.BoundSession.Send(presence).Async();
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
