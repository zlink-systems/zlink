using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Region;

[ZLinkSpotActorSendHandler(nameof(EnterRegionRequest))]
public sealed class ReconnectRegion
    : IZLinkSpotActorSendHandler<RegionSpot, PlayerActor, EnterRegionRequest>
{
    public ValueTask HandleAsync(
        RegionSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        EnterRegionRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity mismatch.");
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(MoveRequest))]
public sealed class MovePlayer : IZLinkSpotActorSendHandler<RegionSpot, PlayerActor, MoveRequest>
{
    public ValueTask HandleAsync(
        RegionSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MoveRequest request,
        CancellationToken ct
    )
    {
        return spot.MoveAsync(actor, request, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(ViewRequest))]
public sealed class QueryRegion : IZLinkSpotActorSendHandler<RegionSpot, PlayerActor, ViewRequest>
{
    public ValueTask HandleAsync(
        RegionSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ViewRequest request,
        CancellationToken ct
    )
    {
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(RegionView))]
public sealed class DeliverRegion : IZLinkSpotActorSendHandler<RegionSpot, PlayerActor, RegionView>
{
    public ValueTask HandleAsync(
        RegionSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        RegionView request,
        CancellationToken ct
    )
    {
        return actor.Context.BoundSession.Send(request).Async();
    }
}

[ZLinkSpotActorSendHandler(nameof(OpenDungeonRequest))]
public sealed class OpenDungeon
    : IZLinkSpotActorSendHandler<RegionSpot, PlayerActor, OpenDungeonRequest>
{
    public ValueTask HandleAsync(
        RegionSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        OpenDungeonRequest request,
        CancellationToken ct
    )
    {
        return spot.OpenAsync(actor, request.DungeonId, ct);
    }
}
