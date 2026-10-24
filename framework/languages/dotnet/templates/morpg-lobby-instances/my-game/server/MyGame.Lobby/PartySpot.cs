using System.Text.Json;
using MyGame.Lobby.Domain;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Lobby;

public sealed class PartySpot(
    IZLinkSpotContext context,
    IZLinkActorClient actors,
    IZLinkSpotManager spots
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private PartyState state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DepartedDungeonView, PlayerActor>(nameof(DungeonView));
        Context.Handlers.AddActorPacket<PartyReconnect, PlayerActor>(nameof(JoinLobbyRequest));
        Context.Handlers.AddActorPacket<PartyQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<Ready, PlayerActor>(nameof(ReadyRequest));
        Context.Handlers.AddActorPacket<EnterDungeon, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravel, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<PartyDelivery, PlayerActor>(nameof(PartyView));
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var join = request.Decode<PartyJoin>();
        return ValueTask.FromResult(
            GameNames.PartyId(join.PartyId) == Context.SpotId && state.Join(actorId, join)
                ? ZLinkSpotActorJoinResult.Accept()
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) =>
        BroadcastAsync(ct);

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct)
    {
        state.Leave(actor.Context.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(state.View()).Async();

    private async ValueTask BroadcastAsync(CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var view = state.View();
        foreach (var id in state.Members)
            await actors.SendToActor(id, view).Async();
    }

    public ValueTask ReadyAsync(PlayerActor actor, CancellationToken ct)
    {
        state.MarkReady(actor.Context.ActorId);
        return BroadcastAsync(ct);
    }

    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        if (state.EntryRejection(actor.Context.ActorId) is { } reason)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
            return;
        }
        var dungeonId = GameNames.DungeonId(state.PartyId);
        var created = await spots
            .GetOrCreate(dungeonId, GameNames.DungeonType)
            .InMesh(GameNames.WorldMesh)
            .Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var members = state.Members.ToArray();
        var prepared = await Context
            .Outbound.RequestToSpot(dungeonId, new PrepareDungeon(state.PartyId, members))
            .Async<DungeonPrepared>(ct);
        if (prepared.DungeonId != dungeonId)
            throw new InvalidDataException("Dungeon preparation identity mismatch.");
        foreach (var id in members)
            await actors.SendToActor(id, new JoinDungeon(dungeonId, state.PartyId)).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<PartyState>(payload.Span)
            ?? throw new InvalidDataException("Missing party relocation state.");
}

public sealed class PartyRelocation : IZLinkSpotRelocationAdapter<PartySpot>
{
    public ValueTask<byte[]> CaptureAsync(PartySpot spot, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Capture());
    }

    public ValueTask RestoreAsync(
        PartySpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
