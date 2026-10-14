using MyGame.Shared;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Channel.Infrastructure.ZLink;

[ZLinkSpotActorSendHandler(nameof(EnterWorldRequest))]
public sealed class EnterZone : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, EnterWorldRequest>
{
    public ValueTask HandleAsync(ZoneSpot spot, PlayerActor actor, IZLinkMessageContext context, EnterWorldRequest request, CancellationToken ct)
    {
        if (request.PlayerId != actor.Context.ActorId) throw new InvalidDataException("Player identity mismatch.");
        return spot.ShowAsync(actor);
    }
}
[ZLinkSpotActorSendHandler(nameof(ChangeChannelRequest))]
public sealed class ChangeChannel : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, ChangeChannelRequest>
{
    public ValueTask HandleAsync(ZoneSpot spot, PlayerActor actor, IZLinkMessageContext context, ChangeChannelRequest request, CancellationToken ct)
    {
        actor.Context.JoinSpot(GameNames.ZoneId(request.Channel), new EnterWorldRequest(actor.Context.ActorId)).Defer();
        return ValueTask.CompletedTask;
    }
}
[ZLinkSpotActorSendHandler(nameof(EarnRequest))]
public sealed class EarnGold : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, EarnRequest>
{
    public async ValueTask HandleAsync(ZoneSpot spot, PlayerActor actor, IZLinkMessageContext context, EarnRequest request, CancellationToken ct)
    {
        await actor.EarnAsync(request.Amount, ct);
        await spot.ShowAsync(actor);
    }
}
[ZLinkSpotActorSendHandler(nameof(ViewRequest))]
public sealed class ViewPlayer : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, ViewRequest>
{
    public ValueTask HandleAsync(ZoneSpot spot, PlayerActor actor, IZLinkMessageContext context, ViewRequest request, CancellationToken ct) => spot.ShowAsync(actor);
}
[ZLinkSpotActorSendHandler(nameof(PlayerView))]
public sealed class DeliverView : IZLinkSpotActorSendHandler<ZoneSpot, PlayerActor, PlayerView>
{
    public ValueTask HandleAsync(ZoneSpot spot, PlayerActor actor, IZLinkMessageContext context, PlayerView request, CancellationToken ct) => actor.Context.BoundSession.Send(request).Async();
}

public sealed class ZoneEntry(IZLinkEntrySpotContext context) : IZLinkEntrySpot<PlayerActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;
    public void Configure() => Context.Handlers.AddActorPacket<EnterPlayer, PlayerActor>(nameof(EnterWorldRequest));
    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
}
[ZLinkSpotActorSendHandler(nameof(EnterWorldRequest))]
public sealed class EnterPlayer : IZLinkEntrySpotActorSendHandler<ZoneEntry, PlayerActor, EnterWorldRequest>
{
    public ValueTask HandleAsync(ZoneEntry spot, PlayerActor actor, IZLinkMessageContext context, EnterWorldRequest request, CancellationToken ct)
    {
        if (request.PlayerId != actor.Context.ActorId) throw new InvalidDataException("Player identity mismatch.");
        actor.Context.JoinSpot(GameNames.ZoneId(GameNames.FirstChannel), request).Defer();
        return ValueTask.CompletedTask;
    }
}
