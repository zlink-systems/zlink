using System.Text.Json;
using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Table;

public sealed class PlayerActor(
    IZLinkActorContext context,
    IZLinkSpotClient client,
    IPlayerRecords players
) : IZLinkActor
{
    public IZLinkActorContext Context { get; } = context;
    private PlayerMatchState state = new();

    public async ValueTask FindAsync(CancellationToken ct)
    {
        if (state.Found is { } found)
        {
            await Context.BoundSession.Send(found).Async();
            return;
        }
        if (state.TicketId is null)
        {
            await LoadChipsAsync(ct);
            var queued = await RequestPool(
                    new MatchRequest(Guid.NewGuid().ToString("N"), Context.ActorId)
                )
                .Async<MatchQueued>(ct);
            state.TicketId = queued.TicketId;
        }
        await Context.BoundSession.Send(new MatchQueued(state.TicketId)).Async();
    }

    public async ValueTask CancelAsync(string ticketId, string reason, CancellationToken ct)
    {
        if (state.Found is not null || state.TicketId is not { } active || ticketId != active)
            return;
        var cancelled = await RequestPool(new CancelTicket(ticketId, reason))
            .Async<MatchCancelled>(ct);
        if (cancelled.TicketId != active)
            return;
        if (reason == MatchNames.Disconnected)
            ClearTicket();
        else
            await NotifyCancellationAsync(cancelled);
    }

    public ValueTask DisconnectAsync(CancellationToken ct) =>
        state.TicketId is { } ticket
            ? CancelAsync(ticket, MatchNames.Disconnected, ct)
            : ValueTask.CompletedTask;

    public async ValueTask NotifyCancellationAsync(MatchCancelled cancelled)
    {
        if (cancelled.TicketId != state.TicketId || state.Found is not null)
            return;
        ClearTicket();
        await Context.BoundSession.Send(cancelled).Async();
    }

    public void Assign(MatchAssigned assignment)
    {
        if (
            assignment.TicketId != state.TicketId
            || state.Found is not null
            || assignment.RoomId != assignment.AssignmentId
            || !state.OfferedAssignments.Add(assignment.AssignmentId)
        )
            return;
        state.Assignment = assignment;
        ScheduleAssignment();
    }

    public async ValueTask JoinFixedAsync(JoinTableRequest request, CancellationToken ct)
    {
        if (state.Joining is not null)
            return;
        var chips = await LoadChipsAsync(ct);
        state.Joining = new JoinAttempt(null);
        Context.JoinSpot(request.TableId, new TableAdmission(request, chips)).Defer();
    }

    private void ScheduleAssignment()
    {
        if (state.Joining is not null || state.Assignment is not { } assignment)
            return;
        state.Joining = new JoinAttempt(assignment);
        var chips =
            state.LoadedChips
            ?? throw new InvalidOperationException("Load the player before requesting a match.");
        Context
            .JoinSpot(assignment.RoomId, new RoomAdmission(assignment.AssignmentId, chips))
            .Defer();
    }

    public async ValueTask OnJoinCompletedAsync(
        ZLinkActorJoinCompletion completion,
        CancellationToken cancellationToken
    )
    {
        if (state.Joining is not { } attempt)
            return;
        state.Joining = null;
        if (attempt.Assignment is { } assignment)
        {
            if (assignment == state.Assignment)
                await client
                    .SendToSpot(
                        MatchNames.PoolKey,
                        new EnterReport(
                            assignment.TicketId,
                            assignment.AssignmentId,
                            completion is ZLinkActorJoinCompletion.Accepted
                        )
                    )
                    .InstanceSpot(MatchNames.PoolType)
                    .InMesh(GameNames.WorldMesh)
                    .Async();
        }
        else if (completion is ZLinkActorJoinCompletion.Rejected)
            await Context.BoundSession.Send(new ActionRejected(Rejection.TableFull, 0)).Async();
        else if (completion is ZLinkActorJoinCompletion.Failed failed)
            throw new InvalidOperationException($"Table join failed: {failed.Kind}.");

        if (state.Assignment != attempt.Assignment)
            ScheduleAssignment();
    }

    public async ValueTask DeliverAsync(TableView view)
    {
        if (
            state.Assignment is { } assignment
            && assignment.RoomId == view.TableId
            && state.Found is null
        )
        {
            var found = new MatchFound(assignment.TicketId, view.TableId, view.YourSeat);
            state.Found = found;
            await Context.BoundSession.Send(found).Async();
        }
        await Context.BoundSession.Send(view).Async();
    }

    public ValueTask DeliverAsync(HandResult result)
    {
        state.LoadedChips = result.Seats.Single(seat => seat.PlayerId == Context.ActorId).Chips;
        return Context.BoundSession.Send(result).Async();
    }

    // Use the public node-level client for both Entry and Table actors.
    private IZLinkSpotRequestCall RequestPool<T>(T request) =>
        client
            .RequestToSpot(MatchNames.PoolKey, request)
            .InstanceSpot(MatchNames.PoolType)
            .InMesh(GameNames.WorldMesh);

    private async ValueTask<long> LoadChipsAsync(CancellationToken ct) =>
        state.LoadedChips ??= await players.LoadPlayerAsync(Context.ActorId, ct);

    private void ClearTicket()
    {
        state.TicketId = null;
        state.Assignment = null;
        state.Found = null;
        state.OfferedAssignments.Clear();
    }

    internal byte[] Capture() => JsonSerializer.SerializeToUtf8Bytes(state);

    internal void Restore(ReadOnlyMemory<byte> payload) =>
        state = payload.IsEmpty
            ? new()
            : JsonSerializer.Deserialize<PlayerMatchState>(payload.Span)
                ?? throw new InvalidDataException("Missing Player match state.");
}

internal sealed record JoinAttempt(MatchAssigned? Assignment);

internal sealed record PlayerMatchState
{
    public long? LoadedChips { get; set; }
    public string? TicketId { get; set; }
    public MatchAssigned? Assignment { get; set; }
    public HashSet<string> OfferedAssignments { get; init; } = [];
    public JoinAttempt? Joining { get; set; }
    public MatchFound? Found { get; set; }
}

public sealed class PlayerFactory(IZLinkSpotClient client, IPlayerRecords players)
    : IZLinkActorFactory<PlayerActor>
{
    public ValueTask<PlayerActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken
    ) => ValueTask.FromResult(new PlayerActor(context, client, players));
}

public sealed class PlayerRelocation : IZLinkActorRelocationAdapter<PlayerActor>
{
    public ValueTask<byte[]> CaptureAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.FromResult(actor.Capture());

    public ValueTask RestoreAsync(
        PlayerActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken
    )
    {
        actor.Restore(payload);
        return ValueTask.CompletedTask;
    }
}
