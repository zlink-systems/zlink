using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Match;

public sealed class RoomReconnectHandler
    : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, JoinRoomRequest>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinRoomRequest request,
        CancellationToken ct
    )
    {
        if (actor.Context.ActorId != request.PlayerId || spot.Context.SpotId != request.RoomId)
            throw new InvalidDataException("Reconnect identity mismatch.");
        return spot.ViewAsync(actor);
    }
}

public sealed class RoomViewHandler
    : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, RoomViewRequest>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        RoomViewRequest request,
        CancellationToken ct
    )
    {
        return spot.ViewAsync(actor);
    }
}

public sealed class MoveHandler : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, MoveRequest>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MoveRequest request,
        CancellationToken ct
    )
    {
        return spot.MoveAsync(actor, request, ct);
    }
}

public sealed class ShootHandler : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, ShootRequest>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        ShootRequest request,
        CancellationToken ct
    )
    {
        return spot.ShootAsync(actor, request, ct);
    }
}

public sealed class RoomViewDelivery : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, RoomView>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        RoomView message,
        CancellationToken ct
    ) => actor.Context.BoundSession.Send(message).Async();
}

public sealed class MatchEndedDelivery
    : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, MatchEnded>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        MatchEnded message,
        CancellationToken ct
    ) => actor.Context.BoundSession.Send(message).Async();
}
