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
        Context.Handlers.AddActorPacket<DungeonReconnect, PlayerActor>(nameof(JoinTownRequest));
        Context.Handlers.AddActorPacket<DungeonEnter, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravelRepeat, PlayerActor>(nameof(JoinDungeon));
        Context.Handlers.AddActorPacket<DungeonChangeChannel, PlayerActor>(nameof(ChangeChannelRequest));
        Context.Handlers.AddActorPacket<DungeonQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<Advance, PlayerActor>(nameof(AdvanceRequest));
        Context.Handlers.AddActorPacket<DungeonDelivery, PlayerActor>(nameof(DungeonView));
        Context.Handlers.AddActorPacket<TownDelivery, PlayerActor>(nameof(TownView));
    }

    public DungeonPrepared Prepare(PrepareDungeon request)
    {
        var reason = state.Prepare(Context.SpotId, request);
        return new DungeonPrepared(Context.SpotId, request.RunId, reason);
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var join = request.Decode<JoinDungeon>();
        return ValueTask.FromResult(
            state.Accepts(actorId, Context.SpotId, join)
                ? ZLinkSpotActorJoinResult.Accept()
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        if (state.Join(actor.Context.ActorId))
            await Context.Outbound.SendToSpot(state.OwnerSpotId, new RunStarted(state.RunId)).Async();
        await BroadcastAsync();
    }

    private async ValueTask BroadcastAsync()
    {
        foreach (var id in state.Joined) await actors.SendToActor(id, View()).Async();
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private DungeonView View() =>
        new(Context.SpotId, settings.Id, state.PartyId, state.Members, state.Stage, state.RunId, state.Phase);

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    public async ValueTask ShowWithPartyAsync(PlayerActor actor)
    {
        await Context.Outbound.SendToSpot(
            state.OwnerSpotId,
            new PartyViewRequest(actor.Context.ActorId)
        ).Async();
        await ShowAsync(actor);
    }

    public async ValueTask AdvanceAsync(PlayerActor actor, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        var reason = state.Advance();
        if (reason is not null)
        {
            await actor.Context.BoundSession.Send(new ActionRejected(reason)).Async();
            return;
        }
        await BroadcastAsync();
        if (state.Phase == RunPhase.Completed)
            await Context.Outbound.SendToSpot(state.OwnerSpotId, new RunCompleted(state.RunId)).Async();
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
