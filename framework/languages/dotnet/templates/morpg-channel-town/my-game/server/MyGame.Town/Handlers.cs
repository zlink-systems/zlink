using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Town;

[ZLinkSpotActorSendHandler(nameof(JoinTownRequest))]
public sealed class TownReconnect
    : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, JoinTownRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinTownRequest request,
        CancellationToken ct
    )
    {
        if (request.PlayerId != actor.Context.ActorId)
            throw new InvalidDataException("Player identity mismatch.");
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(ChangeChannelRequest))]
public sealed class ChangeChannel
    : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, ChangeChannelRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ChangeChannelRequest request,
        CancellationToken ct
    )
    {
        actor
            .Context.JoinSpot(
                GameNames.TownId(request.Channel),
                new JoinTownRequest(actor.Context.ActorId, request.Channel)
            )
            .Defer();
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorSendHandler(nameof(CreatePartyRequest))]
public sealed class CreateParty
    : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, CreatePartyRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        CreatePartyRequest request,
        CancellationToken ct
    )
    {
        return spot.PartyAsync(actor, request.PartyId, true, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(JoinPartyRequest))]
public sealed class JoinParty : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, JoinPartyRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinPartyRequest request,
        CancellationToken ct
    )
    {
        return spot.PartyAsync(actor, request.PartyId, false, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(EnterDungeonRequest))]
public sealed class EnterDungeon
    : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        EnterDungeonRequest request,
        CancellationToken ct
    )
    {
        return spot.EnterAsync(actor, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(JoinDungeon))]
public sealed class DungeonTravel : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinDungeon request,
        CancellationToken ct
    )
    {
        actor.Context.JoinSpot(request.DungeonId, request).Defer();
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorSendHandler(nameof(ViewRequest))]
public sealed class TownQuery : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, ViewRequest>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ViewRequest request,
        CancellationToken ct
    )
    {
        return spot.ShowAsync(actor);
    }
}

[ZLinkSpotActorSendHandler(nameof(TownView))]
public sealed class TownDelivery : IZLinkSpotActorSendHandler<TownSpot, PlayerActor, TownView>
{
    public ValueTask HandleAsync(
        TownSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        TownView request,
        CancellationToken ct
    )
    {
        return actor.Context.BoundSession.Send(request).Async();
    }
}
