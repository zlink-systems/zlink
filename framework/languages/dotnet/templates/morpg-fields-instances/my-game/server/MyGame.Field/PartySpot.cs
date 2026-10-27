using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Field;

// Players remain members of their Field or Dungeon; this Spot owns only the party record.
public sealed class PartySpot(IZLinkSpotContext context, IZLinkActorClient actors,
    IZLinkSpotManager spots) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    private PartyRun state = new();
    public void Configure() { }
    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId,
        Zlink.Framework.Contracts.Messaging.ZLinkMessage request, CancellationToken ct) =>
        ValueTask.FromResult(ZLinkSpotActorJoinResult.Reject());
    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;
    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken ct) => ValueTask.CompletedTask;

    public async ValueTask<PartyCommandResult> CommandAsync(PartyCommand command, CancellationToken ct)
    {
        var rejection = command.Join ? state.Join(command.ActorId)
            : command.Enter ? state.Enter(command.ActorId)
            : state.Members.Contains(command.ActorId) ? null : new ActionRejected("invalid-party");
        if (rejection is not null) return new(null, rejection);
        if (command.Enter && state.Phase != RunPhase.Active)
        {
            var runId = state.RunId!;
            var dungeonId = GameNames.DungeonId(runId);
            var created = await spots.GetOrCreate(dungeonId, GameNames.DungeonType)
                .InMesh(GameNames.WorldMesh).Async(ct);
            if (created.State == ZLinkSpotCreateState.Rejected)
                throw new InvalidOperationException("Dungeon creation rejected.");
            var prepared = await Context.Outbound.RequestToSpot(dungeonId,
                new PrepareDungeon(runId, Context.SpotId, state.Members.ToArray()))
                .Async<DungeonPrepared>(ct);
            if (prepared.Rejection is not null) return new(null, prepared.Rejection);
            if (prepared.DungeonId != dungeonId) throw new InvalidDataException("Dungeon identity mismatch.");
            if (state.Prepared(prepared.RunId) is { } failure) return new(null, failure);
            foreach (var id in state.Members.Where(id => !state.Entered.Contains(id)))
                await actors.SendToActor(id, new JoinDungeon(dungeonId, runId)).Async();
        }
        if (command.Join || command.Enter) await BroadcastAsync();
        return new(state.View(Context.SpotId), null);
    }

    public async ValueTask<PartyCommandResult> ProgressAsync(string runId, string? actorId,
        bool completed = false)
    {
        var rejection = actorId is not null ? state.Arrived(runId, actorId)
            : completed ? state.Completed(runId) : state.Started(runId);
        if (rejection is not null) return new(null, rejection);
        await BroadcastAsync();
        return new(state.View(Context.SpotId), null);
    }

    private async ValueTask BroadcastAsync()
    {
        var view = state.View(Context.SpotId);
        foreach (var id in state.Members) await actors.SendToActor(id, view).Async();
    }
    public byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);
    public void Restore(ReadOnlyMemory<byte> bytes) => state = JsonSerializer.Deserialize<PartyRun>(bytes.Span)
        ?? throw new InvalidDataException("Missing party state.");
}

[ZLinkSpotRequestHandler(nameof(PartyCommand))]
public sealed class PartyCommandHandler : IZLinkSpotRequestHandler<PartySpot, PartyCommand, PartyCommandResult>
{
    public ValueTask<PartyCommandResult> HandleAsync(PartySpot spot, PartyCommand request,
        CancellationToken ct) => spot.CommandAsync(request, ct);
}
[ZLinkSpotRequestHandler(nameof(DungeonMemberEntered))]
public sealed class MemberEnteredHandler : IZLinkSpotRequestHandler<PartySpot, DungeonMemberEntered, PartyCommandResult>
{
    public ValueTask<PartyCommandResult> HandleAsync(PartySpot spot, DungeonMemberEntered request,
        CancellationToken ct) => spot.ProgressAsync(request.RunId, request.ActorId);
}
[ZLinkSpotRequestHandler(nameof(RunStarted))]
public sealed class RunStartedHandler : IZLinkSpotRequestHandler<PartySpot, RunStarted, PartyCommandResult>
{
    public ValueTask<PartyCommandResult> HandleAsync(PartySpot spot, RunStarted request,
        CancellationToken ct) => spot.ProgressAsync(request.RunId, null);
}
[ZLinkSpotRequestHandler(nameof(RunCompleted))]
public sealed class RunCompletedHandler : IZLinkSpotRequestHandler<PartySpot, RunCompleted, PartyCommandResult>
{
    public ValueTask<PartyCommandResult> HandleAsync(PartySpot spot, RunCompleted request,
        CancellationToken ct) => spot.ProgressAsync(request.RunId, null, true);
}
public sealed class PartyRelocation : IZLinkSpotRelocationAdapter<PartySpot>
{
    public ValueTask<byte[]> CaptureAsync(PartySpot spot, CancellationToken ct) => ValueTask.FromResult(spot.Capture());
    public ValueTask RestoreAsync(PartySpot spot, ReadOnlyMemory<byte> bytes, CancellationToken ct)
    {
        spot.Restore(bytes);
        return ValueTask.CompletedTask;
    }
}
