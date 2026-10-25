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

// Concrete closed handlers permit public assembly scanning on all supported packages.
public abstract class PlayerDelivery<T> : IZLinkSpotActorSendHandler<RoomSpot, PlayerActor, T>
{
    public ValueTask HandleAsync(
        RoomSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        T message,
        CancellationToken ct
    ) => actor.Context.BoundSession.Send(message).Async();
}

public sealed class RoomViewDelivery : PlayerDelivery<RoomView>;

public sealed class MatchFoundDelivery : PlayerDelivery<MatchFound>;

public sealed class MatchEndedDelivery : PlayerDelivery<MatchEnded>;

[ZLinkSpotRequestHandler(nameof(PrepareRoom))]
public sealed class PrepareRoomHandler : IZLinkSpotRequestHandler<RoomSpot, PrepareRoom, RoomPrepared>
{
    public ValueTask<RoomPrepared> HandleAsync(RoomSpot spot, PrepareRoom request, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Prepare(request));
    }
}
