using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

[ZLinkSpotActorSendHandler(nameof(JoinWorldRequest))]
public sealed class DungeonReconnect
    : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinWorldRequest>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinWorldRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity mismatch.");
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(ViewRequest))]
public sealed class DungeonQuery : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, ViewRequest>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ViewRequest request,
        CancellationToken ct
    )
    {
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(AdvanceRequest))]
public sealed class Advance : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, AdvanceRequest>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        AdvanceRequest request,
        CancellationToken ct
    )
    {
        return spot.AdvanceAsync(actor, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(DungeonView))]
public sealed class DungeonDelivery
    : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, DungeonView>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        DungeonView request,
        CancellationToken ct
    )
    {
        return actor.Context.BoundSession.Send(request).Async();
    }
}

[ZLinkSpotActorSendHandler(nameof(PartyView))]
public sealed class DungeonPartyDelivery : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, PartyView>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context,
        PartyView request, CancellationToken ct) => actor.Context.BoundSession.Send(request).Async();
}
[ZLinkSpotActorSendHandler(nameof(EnterDungeonRequest))]
public sealed class DungeonEnter : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context,
        EnterDungeonRequest request, CancellationToken ct) => spot.EnterAsync(actor, ct);
}
[ZLinkSpotActorSendHandler(nameof(JoinDungeon))]
public sealed class DungeonTravelAgain : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context,
        JoinDungeon request, CancellationToken ct)
    {
        if (request.DungeonId != spot.Context.SpotId)
            actor.Context.JoinSpot(request.DungeonId, request).Defer();
        return ValueTask.CompletedTask;
    }
}
