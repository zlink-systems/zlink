using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Channel.Infrastructure.ZLink;

public sealed class ZoneSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterZone, PlayerActor>(nameof(EnterWorldRequest));
        Context.Handlers.AddActorPacket<ChangeChannel, PlayerActor>(nameof(ChangeChannelRequest));
        Context.Handlers.AddActorPacket<EarnGold, PlayerActor>(nameof(EarnRequest));
        Context.Handlers.AddActorPacket<ViewPlayer, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<DeliverView, PlayerActor>(nameof(PlayerView));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var enter = request.Decode<EnterWorldRequest>();
        return ValueTask.FromResult(
            enter.PlayerId == actorId
                ? ZLinkSpotActorJoinResult.Accept()
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public PlayerView View(PlayerActor actor) =>
        new(settings.WorldId, GameNames.Channel(Context.SpotId), settings.Id, actor.Gold.Value);

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View(actor)).Async();

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        actors.SendToActor(actor.Context.ActorId, View(actor)).Async();

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;
}

// The Zone owns no application state. Framework transfers its membership and logical identity.
public sealed class ZoneRelocation : IZLinkSpotRelocationAdapter<ZoneSpot>
{
    public ValueTask<byte[]> CaptureAsync(ZoneSpot spot, CancellationToken ct) =>
        ValueTask.FromResult(Array.Empty<byte>());

    public ValueTask RestoreAsync(
        ZoneSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    ) => ValueTask.CompletedTask;
}
