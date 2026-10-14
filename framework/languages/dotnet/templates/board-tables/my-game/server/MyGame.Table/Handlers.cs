using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

[ZLinkSpotActorSendHandler(nameof(JoinTableRequest))]
public sealed class ReconnectHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, JoinTableRequest>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        JoinTableRequest request,
        CancellationToken ct
    )
    {
        if (actor.Context.ActorId != request.PlayerId || spot.Context.SpotId != request.TableId)
            throw new InvalidDataException("Reconnect identity mismatch.");
        return spot.ViewAsync(actor, ct);
    }
}

[ZLinkSpotActorSendHandler(nameof(TableViewRequest))]
public sealed class ViewHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, TableViewRequest>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        TableViewRequest request,
        CancellationToken ct
    ) => spot.ViewAsync(actor, ct);
}

[ZLinkSpotActorSendHandler(nameof(StartHandRequest))]
public sealed class StartHandler
    : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, StartHandRequest>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        StartHandRequest request,
        CancellationToken ct
    ) => spot.StartAsync(actor, ct);
}

[ZLinkSpotActorSendHandler(nameof(BetRequest))]
public sealed class BetHandler : IZLinkSpotActorSendHandler<TableSpot, PlayerActor, BetRequest>
{
    public ValueTask HandleAsync(
        TableSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        BetRequest request,
        CancellationToken ct
    ) => spot.BetAsync(actor, request, ct);
}
