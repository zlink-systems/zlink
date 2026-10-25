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
    private readonly Positions positions = new();

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
        if (!positions.Contains(actorId) && positions.Count >= settings.Capacity)
            return ValueTask.FromResult(
                ZLinkSpotActorJoinResult.Reject(
                    ZLinkMessage.From(new RegionFull(Rejection.RegionFull, join.Region, join.Index))
                )
            );
        positions.Add(actorId);
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        BroadcastAsync(ct);

    public async ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct)
    {
        positions.Remove(actor.Context.ActorId);
        await BroadcastAsync(ct);
    }

    // Membership and position survive a disconnected Session until an explicit leave.
    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private RegionView View() =>
        new(
            Context.SpotId,
            settings.Id,
            positions.View()
        );

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    private async ValueTask BroadcastAsync(
        CancellationToken ct,
        string? excludedActorId = null,
        RegionView? current = null
    )
    {
        ct.ThrowIfCancellationRequested();
        var view = current ?? View();
        foreach (var player in view.Players)
            if (player.PlayerId != excludedActorId)
                await actors.SendToActor(player.PlayerId, view).Async();
    }

    public async ValueTask MoveAsync(PlayerActor actor, MoveRequest request, CancellationToken ct)
    {
        var rejection = positions.Move(actor.Context.ActorId, request);
        if (rejection is not null)
        {
            await actor.Context.BoundSession.Send(rejection).Async();
            return;
        }

        var view = View();
        await actor.Context.BoundSession.Send(view).Async();
        await BroadcastAsync(ct, actor.Context.ActorId, view);
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

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(positions.Capture());

    public void Restore(ReadOnlyMemory<byte> payload) =>
        positions.Restore(
            JsonSerializer.Deserialize<PositionsSnapshot>(payload.Span)
                ?? throw new InvalidDataException("Missing Region relocation state.")
        );
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
