using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Instance;

public sealed class DungeonSpot(
    IZLinkSpotContext context,
    NodeSettings settings,
    IZLinkActorClient actors
) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private DungeonState state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DungeonEnter, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravel, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<PartyDelivery, PlayerActor>(nameof(PartyView));
        Context.Handlers.AddActorPacket<RejectionDelivery, PlayerActor>(nameof(ActionRejected));
        Context.Handlers.AddActorPacket<DungeonReconnect, PlayerActor>(nameof(JoinLobbyRequest));
        Context.Handlers.AddActorPacket<DungeonQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<Advance, PlayerActor>(nameof(AdvanceRequest));
        Context.Handlers.AddActorPacket<DungeonDelivery, PlayerActor>(nameof(DungeonView));
    }

    public DungeonPrepared Prepare(PrepareDungeon request)
    {
        if (GameNames.DungeonId(request.RunId) != Context.SpotId || state.Prepare(request) is { })
            throw new InvalidDataException(Rejection.RunMismatch);
        return new DungeonPrepared(Context.SpotId, request.RunId);
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request, CancellationToken ct) =>
        ValueTask.FromResult(state.Admit(actorId, request.Decode<JoinDungeon>(), Context.SpotId)
            ? ZLinkSpotActorJoinResult.Accept() : ZLinkSpotActorJoinResult.Reject());

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        var started = state.Join(actor.Context.ActorId);
        await Context.Outbound.SendToSpot(state.PartyId, new DungeonMemberEntered(state.RunId!, actor.Context.ActorId)).Async();
        if (started)
            await Context.Outbound.SendToSpot(state.PartyId, new RunStarted(state.RunId!)).Async();
        await BroadcastAsync();
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private DungeonView View() =>
        new(Context.SpotId, settings.Id, state.PartyId, state.Members, state.Stage, state.RunId, state.Phase);

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    public async ValueTask AdvanceAsync(PlayerActor actor, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        if (state.Advance() is { } reason)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
            return;
        }
        if (state.Phase == RunPhase.Completed)
            await Context.Outbound.SendToSpot(state.PartyId, new RunCompleted(state.RunId!)).Async();
        await BroadcastAsync();
    }

    public ValueTask EnterAsync(PlayerActor actor) => Context.Outbound.SendToSpot(state.PartyId, new EnterPartyRun(actor.Context.ActorId)).Async();

    private async ValueTask BroadcastAsync()
    {
        var view = View();
        foreach (var id in state.Joined) await actors.SendToActor(id, view).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<DungeonState>(payload.Span)
            ?? throw new InvalidDataException("Missing dungeon relocation state.");
}

[ZLinkSpotRequestHandler(nameof(PrepareDungeon))]
public sealed class Prepare : IZLinkSpotRequestHandler<DungeonSpot, PrepareDungeon, DungeonPrepared>
{
    public ValueTask<DungeonPrepared> HandleAsync(
        DungeonSpot spot,
        PrepareDungeon request,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Prepare(request));
    }
}

public sealed class DungeonRelocation : IZLinkSpotRelocationAdapter<DungeonSpot>
{
    public ValueTask<byte[]> CaptureAsync(DungeonSpot spot, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        return ValueTask.FromResult(spot.Capture());
    }

    public ValueTask RestoreAsync(
        DungeonSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken ct
    )
    {
        ct.ThrowIfCancellationRequested();
        spot.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
