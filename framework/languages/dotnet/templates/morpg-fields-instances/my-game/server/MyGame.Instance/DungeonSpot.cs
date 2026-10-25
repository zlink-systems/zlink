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
    private DungeonRun state = new();

    public void Configure()
    {
        Context.Handlers.AddActorPacket<DungeonReconnect, PlayerActor>(nameof(JoinWorldRequest));
        Context.Handlers.AddActorPacket<DungeonQuery, PlayerActor>(nameof(ViewRequest));
        Context.Handlers.AddActorPacket<Advance, PlayerActor>(nameof(AdvanceRequest));
        Context.Handlers.AddActorPacket<DungeonDelivery, PlayerActor>(nameof(DungeonView));
        Context.Handlers.AddActorPacket<DungeonPartyDelivery, PlayerActor>(nameof(PartyView));
        Context.Handlers.AddActorPacket<DungeonEnter, PlayerActor>(nameof(EnterDungeonRequest));
        Context.Handlers.AddActorPacket<DungeonTravelAgain, PlayerActor>(nameof(JoinDungeon));
    }

    public DungeonPrepared Prepare(PrepareDungeon request)
    {
        var rejection = GameNames.DungeonId(request.RunId) != Context.SpotId
            ? new ActionRejected("run-mismatch") : state.Prepare(request);
        return new DungeonPrepared(Context.SpotId, request.RunId, rejection);
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken ct
    )
    {
        var join = request.Decode<JoinDungeon>();
        return ValueTask.FromResult(
            join.DungeonId == Context.SpotId
            && state.CanJoin(actorId, join.RunId)
                ? ZLinkSpotActorJoinResult.Accept()
                : ZLinkSpotActorJoinResult.Reject()
        );
    }

    public async ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct)
    {
        state.Arrived(actor.Context.ActorId);
        var entered = await Context.Outbound.RequestToSpot(state.PartyId,
            new DungeonMemberEntered(state.RunId, actor.Context.ActorId)).Async<PartyCommandResult>(ct);
        if (entered.Rejection is not null) throw new InvalidDataException(entered.Rejection.Reason);
        if (state.Phase == RunPhase.Active)
        {
            var started = await Context.Outbound.RequestToSpot(state.PartyId,
                new RunStarted(state.RunId)).Async<PartyCommandResult>(ct);
            if (started.Rejection is not null) throw new InvalidDataException(started.Rejection.Reason);
            foreach (var id in state.Members) await actors.SendToActor(id, View()).Async();
        }
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    public ValueTask OnDisconnectActorAsync(PlayerActor actor, CancellationToken ct) =>
        ValueTask.CompletedTask;

    private DungeonView View() =>
        new(Context.SpotId, settings.Id, state.PartyId, state.Members, state.Stage, state.RunId, state.Phase);

    public ValueTask ShowAsync(PlayerActor actor) =>
        actor.Context.BoundSession.Send(View()).Async();

    public async ValueTask AdvanceAsync(PlayerActor requester, CancellationToken ct)
    {
        ct.ThrowIfCancellationRequested();
        if (state.Advance() is { } rejection)
        {
            await requester.Context.BoundSession.Send(rejection).Async();
            return;
        }
        if (state.Phase == RunPhase.Completed)
        {
            var completed = await Context.Outbound.RequestToSpot(state.PartyId,
                new RunCompleted(state.RunId)).Async<PartyCommandResult>(ct);
            if (completed.Rejection is not null) throw new InvalidDataException(completed.Rejection.Reason);
        }
        var view = View();
        await requester.Context.BoundSession.Send(view).Async();
        foreach (var id in state.Members)
            if (id != requester.Context.ActorId)
                await actors.SendToActor(id, view).Async();
    }

    public async ValueTask EnterAsync(PlayerActor actor, CancellationToken ct)
    {
        var result = await Context.Outbound.RequestToSpot(state.PartyId,
            new PartyCommand(actor.Context.ActorId, Enter: true)).Async<PartyCommandResult>(ct);
        if (result.Rejection is { } rejection) await actor.Context.BoundSession.Send(rejection).Async();
    }

    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    public void Restore(ReadOnlyMemory<byte> payload) =>
        state =
            JsonSerializer.Deserialize<DungeonRun>(payload.Span)
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
