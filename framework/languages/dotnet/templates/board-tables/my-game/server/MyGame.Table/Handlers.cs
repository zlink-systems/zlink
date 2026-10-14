using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

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

public sealed class PlayerDelivery<TSpot, TActor, TMessage>
    : IZLinkSpotActorSendHandler<TSpot, TActor, TMessage>
    where TSpot : class
    where TActor : IZLinkActor
{
    public ValueTask HandleAsync(
        TSpot spot,
        TActor actor,
        IZLinkMessageContext context,
        TMessage message,
        CancellationToken ct
    ) => SendAsync(actor, message);

    public static ValueTask SendAsync(TActor actor, TMessage message)
    {
        // Resolve Framework-owned generation from the Actor at the delivery boundary.
        if (message is TableView view)
            return actor
                .Context.BoundSession.Send(
                    view with
                    {
                        ObjectGeneration = actor.Context.ObjectGeneration,
                    }
                )
                .Async();
        return actor.Context.BoundSession.Send(message).Async();
    }
}
