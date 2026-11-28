namespace Zlink.Framework.Runtime.Streams;

// The Instance Spot execution gate owns all mutations of an incarnation.
internal sealed class ZLinkAdmissionQueue(
    ZLinkAdmissionQueueConfiguration configuration,
    TimeProvider time,
    ulong epoch
)
{
    private const int RateWindowMs = 30_000;
    private const int FallbackWaitPerPositionMs = 1_000;
    private static readonly TimeSpan RateWindow = TimeSpan.FromMilliseconds(RateWindowMs);
    private static readonly TimeSpan ReportValidity = TimeSpan.FromSeconds(3);
    private static readonly TimeSpan PositionInterval = TimeSpan.FromSeconds(5);

    private readonly LinkedList<Waiter> _waiting = new();
    private readonly Dictionary<AdmissionConnection, LinkedListNode<Waiter>> _waiters = [];
    private readonly Dictionary<AdmissionConnection, long> _usedTickets = [];
    private readonly Dictionary<AdmissionNodeIdentity, NodeSnapshot> _reports = [];
    private readonly Dictionary<ulong, Permit> _permits = [];
    private readonly Queue<long> _issuances = new();
    private ulong _nextPermit;
    internal int RunningPermitCount => _permits.Count;

    internal AdmissionEnqueued Enqueue(AdmissionEnqueue request)
    {
        var now = time.GetUtcNow().ToUnixTimeMilliseconds();
        Prune(now);
        if (!_waiters.TryGetValue(request.Connection, out var node))
        {
            node = _waiting.AddLast(new Waiter(request.Connection));
            _waiters.Add(request.Connection, node);
        }

        Issue(now);
        return new AdmissionEnqueued(
            epoch,
            Deliver(node.Value, node.Value.Ticket is null ? PositionOf(node.Value) : 0)
        );
    }

    internal async ValueTask<AdmissionReported> ReportAsync(
        AdmissionReport report,
        Func<AdmissionOwnerLease[], CancellationToken, ValueTask> reclaimExpiredOwners,
        CancellationToken cancellationToken
    )
    {
        var now = time.GetUtcNow().ToUnixTimeMilliseconds();
        Prune(now);
        var identity = new AdmissionNodeIdentity(report.NodeRid, report.NodeLifecycleGeneration);
        _reports[identity] = new NodeSnapshot(time.GetTimestamp(), report.Limit, report.InFlight);
        foreach (var used in report.UsedTickets)
        {
            var connection = identity.Connection(used);
            if (_waiters.TryGetValue(connection, out var node) && node.Value.Ticket is not null)
            {
                _usedTickets[connection] = node.Value.ExpiresAt;
                Remove(node);
            }
        }

        foreach (var ended in report.EndedSessions)
        {
            var connection = identity.Connection(ended);
            if (_waiters.TryGetValue(connection, out var node))
                Remove(node);
            _usedTickets.Remove(connection);
        }

        var held = report.HeldPermits.ToHashSet();
        foreach (var (id, permit) in _permits)
            if (
                permit.Node == identity
                && permit.RequestSeq <= report.ResolvedRequestSeq
                && !held.Contains(id)
            )
                _permits.Remove(id);
        if (Issue(now) && _permits.Count != 0)
        {
            await reclaimExpiredOwners(PermitOwners(), cancellationToken).ConfigureAwait(false);
            now = time.GetUtcNow().ToUnixTimeMilliseconds();
            Prune(now);
            Issue(now);
        }

        var deliveries = new List<AdmissionDelivery>();
        foreach (var (waiter, position) in PositionedWaiters())
        {
            if (waiter.Identity != identity)
                continue;
            if (waiter.Ticket is not null)
            {
                if (!waiter.TicketDelivered)
                    deliveries.Add(Deliver(waiter, position));
                continue;
            }

            var estimate = Estimate(position, _issuances.Count);
            if (
                waiter.LastPosition != position
                || waiter.LastEstimate != estimate
                || time.GetElapsedTime(waiter.LastDelivery) >= PositionInterval
            )
                deliveries.Add(Deliver(waiter, position));
        }

        return new AdmissionReported(epoch, deliveries.ToArray());
    }

    internal AdmissionPermitResult Admit(AdmissionPermitRequest request)
    {
        if (request.Epoch != epoch || configuration.GlobalLimit is null)
            return new AdmissionPermitResult(false);

        var now = time.GetUtcNow().ToUnixTimeMilliseconds();
        Prune(now);
        var connection = PermitConnection(request);
        _waiters.TryGetValue(connection, out var node);
        if (!HasValidTicket(connection, node, now))
            return new AdmissionPermitResult(false);

        if (IsAtLimit)
        {
            _usedTickets.Remove(connection);
            if (node is not null)
                Remove(node);
            var waiter = new Waiter(connection);
            var front = _waiting.First;
            while (front is not null && front.Value.Identity != waiter.Identity)
                front = front.Next;
            _waiters.Add(
                connection,
                front is null ? _waiting.AddLast(waiter) : _waiting.AddBefore(front, waiter)
            );
            return new AdmissionPermitResult(false);
        }

        // Release carries the permit ID without an epoch, so start each incarnation's
        // sequence at its random epoch.
        ulong id;
        do
        {
            id = unchecked(epoch + checked(++_nextPermit));
        } while (id == 0);
        _permits.Add(
            id,
            new Permit(
                new AdmissionNodeIdentity(request.NodeRid, request.NodeLifecycleGeneration),
                new AdmissionOwnerLease(request.OwnerId, request.LeaseGeneration),
                request.RequestSeq
            )
        );
        return new AdmissionPermitResult(true, id);
    }

    internal void Release(AdmissionPermitRelease request)
    {
        if (
            _permits.TryGetValue(request.PermitId, out var permit)
            && permit.Node
                == new AdmissionNodeIdentity(request.NodeRid, request.NodeLifecycleGeneration)
        )
            _permits.Remove(request.PermitId);
    }

    private bool IsAtLimit => configuration.GlobalLimit is { } limit && _permits.Count >= limit;

    internal AdmissionOwnerLease[] PermitOwnersAtLimit(AdmissionPermitRequest request)
    {
        if (!IsAtLimit || request.Epoch != epoch)
            return [];

        var connection = PermitConnection(request);
        _waiters.TryGetValue(connection, out var node);
        return HasValidTicket(connection, node, time.GetUtcNow().ToUnixTimeMilliseconds())
            ? PermitOwners()
            : [];
    }

    private bool HasValidTicket(
        AdmissionConnection connection,
        LinkedListNode<Waiter>? node,
        long now
    ) =>
        node?.Value is { Ticket: not null } waiter && waiter.ExpiresAt > now
        || _usedTickets.TryGetValue(connection, out var expiresAt) && expiresAt > now;

    private static AdmissionConnection PermitConnection(AdmissionPermitRequest request) =>
        new(
            request.NodeRid,
            request.NodeLifecycleGeneration,
            request.StreamName,
            request.SessionRid
        );

    private AdmissionOwnerLease[] PermitOwners() =>
        _permits.Values.Select(static permit => permit.Owner).Distinct().ToArray();

    internal void ReclaimOwner(AdmissionOwnerLease owner)
    {
        foreach (var (id, permit) in _permits)
            if (permit.Owner == owner)
                _permits.Remove(id);
    }

    private bool Issue(long now)
    {
        var globalBudget = configuration.GlobalLimit is { } limit
            ? (long)limit
                - _permits.Count
                - _waiting.Count(static waiter => waiter.Ticket is not null)
            : long.MaxValue;
        foreach (var waiter in _waiting)
        {
            if (
                waiter.Ticket is not null
                || !_reports.TryGetValue(waiter.Identity, out var report)
                || Budget(report.Limit, report.InFlight, report.Issued) == 0
            )
                continue;
            if (globalBudget <= 0)
                return true;

            waiter.ExpiresAt = now + ZLinkAdmissionTicket.LifetimeMs;
            waiter.Ticket = configuration.Ticket.Sign(waiter.Connection, epoch, waiter.ExpiresAt);
            report.Issued++;
            globalBudget--;
            _issuances.Enqueue(time.GetTimestamp());
        }
        return false;
    }

    internal static int Budget(int limit, int inFlight, int issued) =>
        (int)Math.Max(0, (long)limit - inFlight - issued);

    internal static int Estimate(int position, int issuances) =>
        (int)
            Math.Min(
                int.MaxValue,
                issuances == 0
                    ? (long)position * FallbackWaitPerPositionMs
                    : ((long)position * RateWindowMs + issuances - 1) / issuances
            );

    private AdmissionDelivery Deliver(Waiter waiter, int position)
    {
        var connection = waiter.Connection;
        if (waiter.Ticket is { } ticket)
        {
            waiter.TicketDelivered = true;
            return new AdmissionDelivery(
                connection.StreamName,
                connection.SessionRid,
                "ticket",
                Ticket: ticket,
                ExpiresInMs: ZLinkAdmissionTicket.LifetimeMs
            );
        }

        var estimate = Estimate(position, _issuances.Count);
        waiter.LastPosition = position;
        waiter.LastEstimate = estimate;
        waiter.LastDelivery = time.GetTimestamp();
        return new AdmissionDelivery(
            connection.StreamName,
            connection.SessionRid,
            "position",
            position,
            estimate
        );
    }

    private int PositionOf(Waiter target)
    {
        foreach (var (waiter, position) in PositionedWaiters())
            if (waiter == target)
                return position;
        throw new InvalidOperationException("The admission queue waiter is missing.");
    }

    private IEnumerable<(Waiter Waiter, int Position)> PositionedWaiters()
    {
        var position = 0;
        foreach (var waiter in _waiting)
            yield return (waiter, waiter.Ticket is null ? ++position : 0);
    }

    private void Prune(long now)
    {
        while (_issuances.TryPeek(out var issuedAt) && time.GetElapsedTime(issuedAt) >= RateWindow)
            _issuances.Dequeue();

        foreach (var (identity, report) in _reports)
            if (time.GetElapsedTime(report.Timestamp) > ReportValidity)
                _reports.Remove(identity);

        foreach (var (connection, expiresAt) in _usedTickets)
            if (expiresAt <= now)
                _usedTickets.Remove(connection);

        for (var node = _waiting.First; node is not null; )
        {
            var next = node.Next;
            if (node.Value.Ticket is not null && node.Value.ExpiresAt <= now)
                Remove(node);
            node = next;
        }
    }

    private void Remove(LinkedListNode<Waiter> node)
    {
        _waiters.Remove(node.Value.Connection);
        _waiting.Remove(node);
    }

    private sealed class Waiter(AdmissionConnection connection)
    {
        internal AdmissionConnection Connection { get; } = connection;
        internal AdmissionNodeIdentity Identity { get; } =
            new(connection.NodeRid, connection.NodeLifecycleGeneration);
        internal string? Ticket { get; set; }
        internal long ExpiresAt { get; set; }
        internal bool TicketDelivered { get; set; }
        internal int LastPosition { get; set; }
        internal int LastEstimate { get; set; }
        internal long LastDelivery { get; set; }
    }

    private sealed record NodeSnapshot(long Timestamp, int Limit, int InFlight)
    {
        internal int Issued { get; set; }
    }

    private sealed record Permit(
        AdmissionNodeIdentity Node,
        AdmissionOwnerLease Owner,
        ulong RequestSeq
    );
}

internal readonly record struct AdmissionNodeIdentity(string NodeRid, ulong Generation)
{
    internal AdmissionConnection Connection(AdmissionSession session) =>
        new(NodeRid, Generation, session.StreamName, session.SessionRid);
}

internal readonly record struct AdmissionOwnerLease(string OwnerId, ulong Generation);
