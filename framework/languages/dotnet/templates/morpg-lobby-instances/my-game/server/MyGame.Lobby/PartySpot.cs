using System.Text.Json;
using MyGame.Lobby.Domain;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
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
        Context.Handlers.AddActorPacket<RejectionDelivery, PlayerActor>(nameof(ActionRejected));
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
            join.PartyId == Context.SpotId && state.Join(actorId, join)
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

    public ValueTask EnterAsync(PlayerActor actor, CancellationToken ct) => EnterAsync(actor.Context.ActorId, ct);

    public async ValueTask EnterAsync(string playerId, CancellationToken ct)
    {
        if (state.Enter(playerId) is { } reason)
        {
            await actors.SendToActor(playerId, new ActionRejected(reason)).Async();
            return;
        }
        if (state.Phase == RunPhase.Active)
        {
            await actors.SendToActor(playerId, state.View()).Async();
            return;
        }
        var runId = state.RunId!;
        var dungeonId = GameNames.DungeonId(runId);
        var created = await spots.GetOrCreate(dungeonId, GameNames.DungeonType).InMesh(GameNames.WorldMesh).Async(ct);
        if (created.State == ZLinkSpotCreateState.Rejected)
            throw new InvalidOperationException("Dungeon creation rejected.");
        var prepared = await Context.Outbound.RequestToSpot(dungeonId,
            new PrepareDungeon(runId, state.PartyId, state.Members.ToArray())).Async<DungeonPrepared>(ct);
        if (prepared.DungeonId != dungeonId || state.Prepared(prepared.RunId) is not null)
            throw new InvalidDataException(Rejection.RunMismatch);
        foreach (var id in state.Members.Where(id => !state.Entered.Contains(id)))
            await actors.SendToActor(id, new JoinDungeon(dungeonId, runId)).Async();
    }

    public void Arrived(DungeonMemberEntered request)
    {
        if (state.Arrived(request.RunId, request.PlayerId) is { } reason) throw new InvalidDataException(reason);
    }
    public void Started(RunStarted request)
    {
        if (state.Started(request.RunId) is { } reason) throw new InvalidDataException(reason);
    }

    public void Completed(RunCompleted request)
    {
        if (state.Completed(request.RunId) is { } reason) throw new InvalidDataException(reason);
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

[ZLinkSpotPacketHandler(nameof(RunStarted))]
public sealed class Started : IZLinkSpotPacketHandler<PartySpot, RunStarted>
{
    public ValueTask HandleAsync(PartySpot spot, RunStarted request, CancellationToken ct)
    { spot.Started(request); return ValueTask.CompletedTask; }
}

[ZLinkSpotPacketHandler(nameof(RunCompleted))]
public sealed class Completed : IZLinkSpotPacketHandler<PartySpot, RunCompleted>
{
    public ValueTask HandleAsync(PartySpot spot, RunCompleted request, CancellationToken ct)
    { spot.Completed(request); return ValueTask.CompletedTask; }
}

[ZLinkSpotPacketHandler(nameof(EnterPartyRun))]
public sealed class EnterRun : IZLinkSpotPacketHandler<PartySpot, EnterPartyRun>
{
    public ValueTask HandleAsync(PartySpot spot, EnterPartyRun request, CancellationToken ct) => spot.EnterAsync(request.PlayerId, ct);
}

[ZLinkSpotPacketHandler(nameof(DungeonMemberEntered))]
public sealed class Arrived : IZLinkSpotPacketHandler<PartySpot, DungeonMemberEntered>
{
    public ValueTask HandleAsync(PartySpot spot, DungeonMemberEntered request, CancellationToken ct)
    { spot.Arrived(request); return ValueTask.CompletedTask; }
}
