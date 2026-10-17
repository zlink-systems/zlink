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
        return spot.AdvanceAsync(ct);
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
        return actor.DeliverAsync(request.DungeonId, request);
    }
}

public sealed class DepartedPartyView
    : IZLinkSpotActorSendHandler<DungeonSpot, PlayerActor, PartyView>
{
    public ValueTask HandleAsync(
        DungeonSpot spot,
        PlayerActor actor,
        IZLinkMessageContext context,
        PartyView message,
        CancellationToken ct
    ) => actor.DeliverAsync(GameNames.PartyId(message.PartyId), message);
}
