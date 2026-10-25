using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

[ZLinkSpotActorSendHandler(nameof(JoinLobbyRequest))]
public sealed class DungeonReconnect
    : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinLobbyRequest>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinLobbyRequest request,
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

public sealed class RejectionDelivery : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, ActionRejected>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, ActionRejected request, CancellationToken ct)
        => actor.Context.BoundSession.Send(request).Async();
}

public sealed class DungeonEnter : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, EnterDungeonRequest request, CancellationToken ct) => spot.EnterAsync(actor);
}
public sealed class DungeonTravel : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, JoinDungeon request, CancellationToken ct)
    { actor.Context.JoinSpot(request.DungeonId, request).Defer(); return ValueTask.CompletedTask; }
}
public sealed class PartyDelivery : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, PartyView>
{
    public ValueTask HandleAsync(DungeonSpot spot, PlayerActor actor, IZLinkMessageContext context, PartyView request, CancellationToken ct)
        => actor.Context.BoundSession.Send(request).Async();
}
