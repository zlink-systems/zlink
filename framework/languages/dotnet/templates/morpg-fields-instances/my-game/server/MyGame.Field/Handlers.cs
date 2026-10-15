using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace MyGame.Field;

[ZLinkSpotActorSendHandler(nameof(JoinWorldRequest))]
public sealed class ReconnectHandler
    : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, JoinWorldRequest>
{
    public async ValueTask HandleAsync(
        FieldSpot spot,
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
public sealed class MoveHandler : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, MoveRequest>
{
    public async ValueTask HandleAsync(
        FieldSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MoveRequest request,
        CancellationToken cancellationToken
    )
    {
        if (request.Sequence != actor.Sequence + 1)
            throw new InvalidDataException("Move sequence must increase by one.");
        var fieldId = GameNames.FieldId(request.X);
        var position = new EnterField(request.X, request.Sequence);
        if (fieldId != spot.Context.SpotId)
        {
            actor.Move(position);
            return;
        }
        actor.Apply(position);
        spot.Remember(actor);
        await actor.SendStateAsync();
    }
}

public sealed class BorderTick : IZLinkSpotTimerHandler<FieldSpot>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken
    ) => spot.PublishBorderAsync(cancellationToken);
}

public sealed class BorderHandler : IZLinkSpotSubscriptionHandler<FieldSpot, BorderPresence>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
        BorderPresence presence,
        Zlink.Framework.Contracts.Channels.ZLinkPublishMessageContext context,
        CancellationToken cancellationToken
    ) => spot.ShowBorderAsync(presence);
}

[ZLinkSpotActorSendHandler(nameof(BorderVisible))]
public sealed class BorderDelivery
    : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, BorderVisible>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        BorderVisible presence,
        CancellationToken cancellationToken
    ) => actor.Context.BoundSession.Send(presence).Async();
}

[ZLinkSpotActorSendHandler(nameof(CreatePartyRequest))]
public sealed class CreateParty
    : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, CreatePartyRequest>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
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
public sealed class JoinParty : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, JoinPartyRequest>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
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
    : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
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
public sealed class DungeonTravel : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
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

[ZLinkSpotActorSendHandler(nameof(FieldView))]
public sealed class FieldDelivery : IZLinkSpotActorSendHandler<FieldSpot, PlayerActor, FieldView>
{
    public ValueTask HandleAsync(
        FieldSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        FieldView request,
        CancellationToken ct
    )
    {
        return actor.Context.BoundSession.Send(request).Async();
    }
}
