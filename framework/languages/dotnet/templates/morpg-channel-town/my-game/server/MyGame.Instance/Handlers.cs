using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

[ZLinkSpotActorSendHandler(nameof(JoinTownRequest))]
public sealed class DungeonReconnect
    : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinTownRequest>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinTownRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity mismatch.");
        return spot.ShowWithPartyAsync(actor);
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
        return spot.ShowWithPartyAsync(actor);
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

[ZLinkSpotActorSendHandler(nameof(TownView))]
public sealed class TownDelivery : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, TownView>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        TownView request,
        CancellationToken ct
    ) => actor.Context.BoundSession.Send(request).Async();
}

[ZLinkSpotActorSendHandler(nameof(EnterDungeonRequest))]
public sealed class DungeonEnter : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, EnterDungeonRequest request, CancellationToken ct)
        => spot.ShowAsync(actor);
}

[ZLinkSpotActorSendHandler(nameof(JoinDungeon))]
public sealed class DungeonTravelRepeat : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, JoinDungeon request, CancellationToken ct)
        => ValueTask.CompletedTask;
}

[ZLinkSpotActorSendHandler(nameof(ChangeChannelRequest))]
public sealed class DungeonChangeChannel : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, ChangeChannelRequest>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, ChangeChannelRequest request, CancellationToken ct)
        => actor.Context.BoundSession.Send(new ActionRejected(Rejection.RunInProgress)).Async();
}
