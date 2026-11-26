using MyGame.Shared;

namespace MyGame.Matchmaker;

internal sealed record Ticket(string Id, string PlayerId, DateTimeOffset ExpiresAt)
{
    public LinkedListNode<Ticket>? Waiting { get; set; }
    public Assignment? Assignment { get; set; }
    public DateTimeOffset? DisconnectDeadline { get; set; }
}

internal sealed record Assignment(string Id, Ticket[] Members, DateTimeOffset Deadline)
{
    public bool Prepared { get; set; }
    public HashSet<string> Entered { get; } = [];
}

internal sealed record Resolution(
    Assignment Assignment,
    bool Committed,
    (Ticket Ticket, string Reason)[] Cancelled
);

internal sealed class MatchQueue
{
    private readonly LinkedList<Ticket> waiting = new();
    private readonly Dictionary<string, Ticket> tickets = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Ticket> players = new(StringComparer.Ordinal);
    private readonly Dictionary<string, Assignment> assignments = new(StringComparer.Ordinal);

    public Ticket Enqueue(string requestKey, string playerId, DateTimeOffset now)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(requestKey);
        if (players.TryGetValue(playerId, out var active))
        {
            active.DisconnectDeadline = null;
            return active;
        }

        var ticket = new Ticket(Guid.NewGuid().ToString("N"), playerId, now.AddSeconds(120));
        ticket.Waiting = waiting.AddLast(ticket);
        tickets.Add(ticket.Id, ticket);
        players.Add(playerId, ticket);
        return ticket;
    }

    public Assignment? Reserve(DateTimeOffset now)
    {
        var picked = MatchRule.Select(waiting.Where(ticket => ticket.DisconnectDeadline is null));
        if (picked.Length < 2)
            return null;
        var assignment = new Assignment($"room-{Guid.NewGuid():N}", picked, now.AddSeconds(10));
        foreach (var ticket in picked)
        {
            waiting.Remove(ticket.Waiting!);
            ticket.Waiting = null;
            ticket.Assignment = assignment;
        }
        assignments.Add(assignment.Id, assignment);
        return assignment;
    }

    public Assignment? MarkPrepared(RoomPrepared message, DateTimeOffset now)
    {
        if (
            !assignments.TryGetValue(message.AssignmentId, out var assignment)
            || assignment.Prepared
            || now >= assignment.Deadline
        )
            return null;
        if (!message.Ok)
        {
            assignments.Remove(assignment.Id);
            foreach (var ticket in assignment.Members.Reverse())
                Requeue(ticket);
            return null;
        }

        assignment.Prepared = true;
        return assignment;
    }

    public (Ticket? Ticket, Resolution? Resolution) Cancel(
        string id,
        string reason,
        DateTimeOffset now
    )
    {
        if (!tickets.TryGetValue(id, out var ticket))
            return (null, null);
        if (ticket.Assignment is { } assignment)
        {
            if (reason == MatchNames.Disconnected || now >= assignment.Deadline)
                return (null, null);
            return (ticket, Withdraw(assignment, ticket, reason));
        }
        if (reason == MatchNames.Disconnected)
            ticket.DisconnectDeadline ??= now.AddSeconds(15);
        else
            Remove(ticket);
        return (ticket, null);
    }

    public Resolution? Report(string ticketId, string assignmentId, bool ok, DateTimeOffset now)
    {
        if (
            !tickets.TryGetValue(ticketId, out var ticket)
            || ticket.Assignment is not { } assignment
            || assignment.Id != assignmentId
            || !assignment.Prepared
        )
            return null;
        if (now >= assignment.Deadline)
            return Withdraw(assignment, null, MatchNames.Expired);
        if (!ok)
            return Withdraw(assignment, ticket, MatchNames.Cancelled);
        if (
            !assignment.Entered.Add(ticketId)
            || assignment.Entered.Count != assignment.Members.Length
        )
            return null;
        assignments.Remove(assignment.Id);
        foreach (var member in assignment.Members)
            Remove(member);
        return new Resolution(assignment, true, []);
    }

    public (Ticket Ticket, string Reason)[] ExpireWaiting(DateTimeOffset now)
    {
        var expired = waiting
            .Where(ticket => ticket.ExpiresAt <= now || ticket.DisconnectDeadline <= now)
            .Select(ticket =>
                (
                    Ticket: ticket,
                    Reason: ticket.DisconnectDeadline <= now
                    && ticket.DisconnectDeadline <= ticket.ExpiresAt
                        ? MatchNames.Disconnected
                        : MatchNames.Expired
                )
            )
            .ToArray();
        foreach (var (ticket, _) in expired)
            Remove(ticket);
        return expired;
    }

    public Resolution[] ExpireAssignments(DateTimeOffset now) =>
        assignments
            .Values.Where(a => a.Deadline <= now)
            .ToArray()
            .Select(a => Withdraw(a, null, MatchNames.Expired))
            .ToArray();

    private Resolution Withdraw(Assignment assignment, Ticket? failed, string reason)
    {
        assignments.Remove(assignment.Id);
        var cancelled = new List<(Ticket, string)>();
        foreach (var ticket in assignment.Members.Reverse())
        {
            if (failed is null ? assignment.Entered.Contains(ticket.Id) : ticket.Id != failed.Id)
                Requeue(ticket);
            else
            {
                Remove(ticket);
                cancelled.Add((ticket, reason));
            }
        }
        return new Resolution(assignment, false, cancelled.ToArray());
    }

    private void Requeue(Ticket ticket)
    {
        ticket.Assignment = null;
        ticket.Waiting = waiting.AddFirst(ticket);
    }

    private void Remove(Ticket ticket)
    {
        if (ticket.Waiting is { } node)
            waiting.Remove(node);
        players.Remove(ticket.PlayerId);
        tickets.Remove(ticket.Id);
    }
}
