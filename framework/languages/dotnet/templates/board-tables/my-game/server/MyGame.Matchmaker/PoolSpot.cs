using MyGame.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Spots;

namespace MyGame.Matchmaker;

public sealed class PoolSpot(IZLinkInstanceSpotContext context, IZLinkActorClient actors)
    : IZLinkInstanceSpot
{
    private readonly MatchQueue queue = new();
    public IZLinkInstanceSpotContext Context { get; } = context;

    public async ValueTask OnInitializeAsync(CancellationToken ct) =>
        _ = await Context.AddTimer<PoolTick>(
            "match",
            TimeSpan.FromMilliseconds(500),
            cancellationToken: ct
        );

    public async ValueTask<MatchQueued> EnqueueAsync(MatchRequest request)
    {
        var ticket = queue.Enqueue(request.RequestKey, request.PlayerId, DateTimeOffset.UtcNow);
        await AdvanceAsync();
        return new MatchQueued(ticket.Id);
    }

    public async ValueTask<MatchCancelled> CancelAsync(CancelTicket request)
    {
        var (ticket, resolution) = queue.Cancel(
            request.TicketId,
            request.Reason,
            DateTimeOffset.UtcNow
        );
        if (resolution is not null)
            await DeliverAsync(resolution, request.TicketId);
        // An empty ticket ID acknowledges a no-op without cancelling an Actor ticket.
        return new MatchCancelled(ticket?.Id ?? "", request.Reason);
    }

    public async ValueTask ReportAsync(EnterReport report)
    {
        var resolution = queue.Report(
            report.TicketId,
            report.AssignmentId,
            report.Ok,
            DateTimeOffset.UtcNow
        );
        if (resolution is not null)
            await DeliverAsync(resolution);
    }

    public async ValueTask AdvanceAsync()
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var resolution in queue.ExpireAssignments(now))
            await DeliverAsync(resolution);
        foreach (var (ticket, reason) in queue.ExpireWaiting(now))
            await NotifyCancellationAsync(ticket, reason);
        while (queue.Reserve(DateTimeOffset.UtcNow) is { } assignment)
        {
            await Context
                .Outbound.SendToSpot(
                    MatchNames.RoomProvisionerKey,
                    new PrepareRoom(
                        assignment.Id,
                        assignment.Members.Select(ticket => ticket.PlayerId).ToArray()
                    )
                )
                .InstanceSpot(MatchNames.RoomProvisionerType)
                .InMesh(GameNames.WorldMesh)
                .Async();
        }
    }

    public async ValueTask RoomPreparedAsync(RoomPrepared message)
    {
        var assignment = queue.MarkPrepared(message, DateTimeOffset.UtcNow);
        if (assignment is null)
            return;

        foreach (var ticket in assignment.Members)
            await actors
                .SendToActor(
                    ticket.PlayerId,
                    new MatchAssigned(ticket.Id, assignment.Id, assignment.Id)
                )
                .Async();
    }

    private async ValueTask DeliverAsync(Resolution resolution, string? replyTicket = null)
    {
        if (resolution.Committed)
        {
            await Context
                .Outbound.SendToSpot(
                    resolution.Assignment.Id,
                    new CommitRoom(resolution.Assignment.Id)
                )
                .Async();
            return;
        }
        await WithdrawAsync(resolution.Assignment.Id);
        foreach (var (ticket, reason) in resolution.Cancelled)
            if (ticket.Id != replyTicket)
                await NotifyCancellationAsync(ticket, reason);
    }

    private ValueTask WithdrawAsync(string id) =>
        Context
            .Outbound.SendToSpot(MatchNames.RoomProvisionerKey, new WithdrawRoom(id))
            .InstanceSpot(MatchNames.RoomProvisionerType)
            .InMesh(GameNames.WorldMesh)
            .Async();

    private ValueTask NotifyCancellationAsync(Ticket ticket, string reason) =>
        actors.SendToActor(ticket.PlayerId, new MatchCancelled(ticket.Id, reason)).Async();
}
