using System.Text.Json;
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
    private readonly HashSet<string> _playerIds = new(StringComparer.Ordinal);

    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<EnterZone, PlayerActor>(nameof(EnterWorldRequest));
        Context.Handlers.AddActorPacket<ChangeChannel, PlayerActor>(nameof(ChangeChannelRequest));
        Context.Handlers.AddActorPacket<EarnGold, PlayerActor>(nameof(EarnRequest));
        Context.Handlers.AddActorPacket<ViewPlayer, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<ViewChannel, PlayerActor>(nameof(ChannelViewRequest));
        Context.Handlers.AddActorPacket<DeliverView, PlayerActor>(nameof(PlayerView));
        Context.Handlers.AddActorPacket<DeliverChannelView, PlayerActor>(nameof(ChannelView));
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

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        _playerIds.Add(actor.Context.ActorId);
        await actors.SendToActor(actor.Context.ActorId, View(actor)).Async();
        await PublishPlayersAsync();
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        RemovePlayerAsync(actor);

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        RemovePlayerAsync(actor);

    private async ValueTask RemovePlayerAsync(PlayerActor actor)
    {
        if (_playerIds.Remove(actor.Context.ActorId))
            await PublishPlayersAsync();
    }

    public ValueTask ShowPlayersAsync(PlayerActor actor) =>
        actor.Context.BoundSession
            .Send(
                new ChannelView(
                    GameNames.Channel(Context.SpotId),
                    _playerIds.OrderBy(playerId => playerId, StringComparer.Ordinal).ToArray()
                )
            )
            .Async();

    private async ValueTask PublishPlayersAsync()
    {
        var view = new ChannelView(GameNames.Channel(Context.SpotId), CapturePlayerIds());
        foreach (var playerId in _playerIds)
            await actors.SendToActor(playerId, view).Async();
    }

    public string[] CapturePlayerIds() =>
        _playerIds.OrderBy(playerId => playerId, StringComparer.Ordinal).ToArray();

    public void RestorePlayerIds(IEnumerable<string> playerIds)
    {
        _playerIds.Clear();
        foreach (var playerId in playerIds)
            _playerIds.Add(playerId);
    }
}

public sealed record ZoneSnapshot(string[] PlayerIds);

public sealed class ZoneRelocation : IZLinkSpotRelocationAdapter<ZoneSpot>
{
    public ValueTask<byte[]> CaptureAsync(ZoneSpot spot, CancellationToken ct) =>
        ValueTask.FromResult(
            JsonSerializer.SerializeToUtf8Bytes(new ZoneSnapshot(spot.CapturePlayerIds()))
        );

    public ValueTask RestoreAsync(
        ZoneSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        var snapshot = JsonSerializer.Deserialize<ZoneSnapshot>(payload.Span)
            ?? throw new InvalidDataException("Zone relocation state is empty.");
        spot.RestorePlayerIds(snapshot.PlayerIds);
        return ValueTask.CompletedTask;
    }
}
