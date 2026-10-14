using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;
namespace MyGame.Lobby;

public sealed class PartyReconnect : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, JoinLobbyRequest>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, JoinLobbyRequest request, CancellationToken ct)
    { if (request.PlayerId != actor.Context.ActorId) throw new InvalidDataException("Player identity mismatch."); return spot.ShowAsync(actor); }
}

public sealed class PartyQuery : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, ViewRequest>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, ViewRequest request, CancellationToken ct)
    { return spot.ShowAsync(actor); }
}

public sealed class Ready : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, ReadyRequest>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, ReadyRequest request, CancellationToken ct)
    { return spot.ReadyAsync(actor, ct); }
}

public sealed class EnterDungeon : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, EnterDungeonRequest>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, EnterDungeonRequest request, CancellationToken ct)
    { return spot.EnterAsync(actor, ct); }
}

public sealed class DungeonTravel : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, JoinDungeon>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, JoinDungeon request, CancellationToken ct)
    { actor.Context.JoinSpot(request.DungeonId, request).Defer(); return ValueTask.CompletedTask; }
}

public sealed class PartyDelivery : IZLinkSpotActorSendHandler<PartySpot, PlayerActor, PartyView>
{
    public ValueTask HandleAsync(PartySpot spot, PlayerActor actor, IZLinkMessageContext context, PartyView request, CancellationToken ct)
    { return actor.Context.BoundSession.Send(request).Async(); }
}
