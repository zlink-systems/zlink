using System.Text.Json;
using MyGame.Region.Domain;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Region;

public sealed class RegionSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors,
    IZLinkSpotManager spots
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private Dictionary<string, PositionState> players = new(StringComparer.Ordinal);

    public void Configure()
    {
        Context.Handlers.AddActorPacket<ReconnectRegion, PlayerActor>(nameof(EnterRegionRequest));
        Context.Handlers.AddActorPacket<MovePlayer, PlayerActor>(nameof(MoveRequest));
        Context.Handlers.AddActorPacket<QueryRegion, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<DeliverRegion, PlayerActor>(nameof(RegionView));
        Context.Handlers.AddActorPacket<OpenDungeon, PlayerActor>(nameof(OpenDungeonRequest));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        var join = request.Decode<JoinRegion>();
        if (GameNames.RegionId(join.Region, join.Index) != Context.SpotId)
            throw new InvalidDataException("Region join identity mismatch.");
        if (!players.ContainsKey(actorId) && players.Count >= settings.Capacity)
            return ValueTask.FromResult(
                ZLinkSpotActorJoinResult.Reject(
                    ZLinkMessage.From(new RegionFull(Rejection.RegionFull, join.Region, join.Index))
                )
            );
        players.TryAdd(actorId, new PositionState());
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        BroadcastAsync(ct);

    public async ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct)
    {
        players.Remove(actor.Context.ActorId);
        await BroadcastAsync(ct);
    }

    // Membership and position survive a disconnected Session until an explicit leave.
    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private RegionView View() =>
        new(
            Context.SpotId,
            settings.Id,
            players
                .OrderBy(pair => pair.Key)
                .Select(pair => new PlayerPosition(pair.Key, pair.Value.X))
                .ToArray()
        );

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    private async ValueTask BroadcastAsync(CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var view = View();
        foreach (var id in players.Keys)
            await actors.SendToActor(id, view).Async();
    }

    public async ValueTask MoveAsync(PlayerActor actor, MoveRequest request, CancellationToken ct)
    {
        var position = players[actor.Context.ActorId];
        if (request.Sequence <= position.Sequence)
            throw new InvalidDataException("Move sequence must increase.");
        position.X = request.X;
        position.Sequence = request.Sequence;
        await BroadcastAsync(ct);
    }

    public async ValueTask OpenAsync(PlayerActor actor, string dungeonId, CancellationToken ct)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(dungeonId);
        var created = await spots
            .GetOrCreate(dungeonId, GameNames.DungeonType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var reply = await Context
            .Outbound.RequestToSpot(dungeonId, new DungeonInfoRequest())
            .Async<DungeonOpened>(ct);
        await actor.Context.BoundSession.Send(reply).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(players);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        players =
            JsonSerializer.Deserialize<Dictionary<string, PositionState>>(payload.Span)
            ?? throw new InvalidDataException("Missing Region relocation state.");
}

public sealed class RegionRelocation : IZLinkSpotRelocationAdapter<RegionSpot>
{
    public ValueTask<byte[]> CaptureAsync(RegionSpot spot, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Capture());
    }

    public ValueTask RestoreAsync(
        RegionSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
