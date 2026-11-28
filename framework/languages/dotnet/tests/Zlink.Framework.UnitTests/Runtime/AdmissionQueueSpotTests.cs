using System.Text;
using System.Text.Json;

namespace Zlink.Framework.UnitTests;

public sealed class AdmissionQueueSpotTests
{
    [Theory]
    [MemberData(nameof(Scenarios))]
    public void QueueScenario(string name, string json)
    {
        using var document = JsonDocument.Parse(json);
        var scenario = document.RootElement;
        var given = scenario.GetProperty("given");
        var steps = scenario.GetProperty("steps").EnumerateArray().ToArray();
        var harness = new QueueHarness(
            given.TryGetProperty("globalLimit", out var limit) ? limit.GetInt32() : null
        );
        switch (name)
        {
            case "enqueue-idempotent":
            {
                var node = given.GetProperty("nodeRid").GetString()!;
                var session = given.GetProperty("sessionRid").GetString()!;
                harness.Enqueue(node, "ahead");
                harness.Enqueue(node, session);
                Assert.Equal(
                    Expected(steps[0], "position").GetInt32(),
                    harness.Enqueue(node, session).Position
                );
                var issued = harness
                    .Report(node, limit: 2)
                    .Deliveries.Single(item => item.SessionRid == Rid(session));
                Assert.Equal("ticket", issued.Kind);
                Assert.Equal(issued.Ticket, harness.Enqueue(node, session).Ticket);
                break;
            }
            case "same-session-rid-on-two-nodes-are-two-waiters":
            {
                foreach (var step in steps.Take(2))
                {
                    var node = step.GetProperty("node").GetString()!;
                    var session = step.GetProperty("sessionRid").GetString()!;
                    Assert.Equal(
                        Expected(step, "position").GetInt32(),
                        harness
                            .Enqueue(
                                node,
                                session,
                                step.GetProperty("nodeLifecycleGeneration").GetUInt64()
                            )
                            .Position
                    );
                }
                var first = steps[0];
                var firstNode = first.GetProperty("node").GetString()!;
                var gen = first.GetProperty("nodeLifecycleGeneration").GetUInt64();
                harness.Report(firstNode, limit: 1, generation: gen);
                harness.Report(firstNode, limit: 1, used: ["a"], generation: gen);
                var remaining = Expected(steps[2], "waiting")[0];
                Assert.Equal(
                    1,
                    harness
                        .Enqueue(
                            remaining.GetProperty("node").GetString()!,
                            remaining.GetProperty("sessionRid").GetString()!,
                            3
                        )
                        .Position
                );
                break;
            }
            case "node-budget-and-fifo":
            case "budget-capped-by-global-limit":
            {
                var report = given.GetProperty("reports")[0];
                var limitAtNode = report.GetProperty("limit").GetInt32();
                var inFlight = report.GetProperty("inFlight").GetInt32();
                var issued = report.GetProperty("issuedSinceReport").GetInt32();
                if (name == "budget-capped-by-global-limit")
                    harness.SeedPermit("other", "p1", 1);

                foreach (var session in given.GetProperty("waiting").EnumerateArray())
                    harness.Enqueue("n", session.GetString()!);

                var tickets = harness
                    .Report("n", limit: limitAtNode, inFlight: inFlight + issued)
                    .Deliveries.Where(item => item.Kind == "ticket")
                    .Select(item => item.SessionRid)
                    .ToArray();
                Assert.Equal(
                    Expected(steps[1], "ticketSessions")
                        .EnumerateArray()
                        .Select(item => Rid(item.GetString()!)),
                    tickets
                );
                Assert.Equal(Expected(steps[0], "budget").GetInt32(), tickets.Length);
                if (name == "budget-capped-by-global-limit")
                {
                    Assert.DoesNotContain(
                        harness.Report("n", limit: limitAtNode).Deliveries,
                        item => item.Kind == "ticket"
                    );
                    harness.Release("other", "p1");
                    Assert.Single(
                        harness
                            .Report("n", limit: limitAtNode)
                            .Deliveries.Where(item => item.Kind == "ticket")
                    );
                }
                break;
            }
            case "stale-report-no-issuance":
            {
                harness.Report("n", limit: 1);
                harness.Time.Advance(
                    TimeSpan.FromMilliseconds(given.GetProperty("reportAgeMs").GetInt32())
                );
                Assert.Equal(
                    "position",
                    harness.Enqueue("n", given.GetProperty("waiting")[0].GetString()!).Kind
                );
                break;
            }
            case "position-skips-waiters-holding-a-ticket":
            {
                foreach (var waiter in given.GetProperty("waiting").EnumerateArray())
                {
                    var node = waiter.GetProperty("node").GetString()!;
                    var session = waiter.GetProperty("sessionRid").GetString()!;
                    if (waiter.GetProperty("ticket").GetBoolean())
                        harness.SeedTicket(node, session, use: false);
                    else
                        harness.Enqueue(node, session);
                }

                foreach (var step in steps)
                {
                    var delivery = harness.Enqueue(
                        step.GetProperty("node").GetString()!,
                        step.GetProperty("sessionRid").GetString()!
                    );
                    var position = Expected(step, "position").GetInt32();
                    Assert.Equal(position, delivery.Position);
                    Assert.Equal(position * 15_000, delivery.EstimatedWaitMs);
                }

                harness.Time.Advance(TimeSpan.FromSeconds(5));
                var pushed = harness.Report("n2").Deliveries;
                Assert.Equal(new[] { Rid("b"), Rid("d") }, pushed.Select(item => item.SessionRid));
                Assert.Equal(new int?[] { 1, 2 }, pushed.Select(item => item.Position));
                Assert.Equal(
                    new int?[] { 15_000, 30_000 },
                    pushed.Select(item => item.EstimatedWaitMs)
                );
                break;
            }
            case "estimated-wait":
                foreach (var step in steps)
                    Assert.Equal(
                        Expected(step, "estimatedWaitMs").GetInt32(),
                        ZLinkAdmissionQueue.Estimate(
                            step.GetProperty("position").GetInt32(),
                            step.GetProperty("ticketsLast30Seconds").GetInt32()
                        )
                    );
                break;
            case "delete-waiters":
            {
                harness.Enqueue("n", "used");
                harness.Report("n", limit: 1);
                harness.Enqueue("n", "ended");
                harness.Enqueue("n", "expired");
                harness.Report("n", limit: 0, used: ["used"]);
                Assert.Equal(
                    Expected(steps[0], "waiters").GetArrayLength(),
                    harness.Enqueue("n", "expired").Position
                );
                harness.Report("n", limit: 0, ended: ["ended"]);
                Assert.Equal(
                    Expected(steps[1], "waiters").GetArrayLength(),
                    harness.Enqueue("n", "expired").Position
                );
                harness.Report("n", limit: 1);
                harness.Time.Advance(TimeSpan.FromMilliseconds(30001));
                Assert.Equal(
                    Expected(steps[2], "waiters").GetArrayLength() + 1,
                    harness.Enqueue("n", "expired").Position
                );
                break;
            }
            case "new-incarnation":
            {
                harness.Enqueue("n", given.GetProperty("waiting")[0].GetString()!);
                var nextEpoch = Expected(steps[0], "epoch").GetUInt64();
                var next = new QueueHarness(null, nextEpoch);
                Assert.Equal(nextEpoch, next.Report("n").Epoch);
                Assert.Empty(next.Report("n").Deliveries);
                var connection = new AdmissionConnection(Rid("n"), 1, "login", Rid("a"));
                var old = harness.Ticket.Sign(
                    connection,
                    given.GetProperty("epoch").GetUInt64(),
                    harness.Time.GetUtcNow().ToUnixTimeMilliseconds() + 30_000
                );
                Assert.Equal(
                    Expected(steps[1], "accept").GetBoolean(),
                    next.Ticket.Verify(
                        old,
                        connection,
                        nextEpoch,
                        next.Time.GetUtcNow().ToUnixTimeMilliseconds()
                    )
                );
                break;
            }
            default:
                VerifyPermitScript(given, steps, harness);
                break;
        }
    }

    private static void VerifyPermitScript(
        JsonElement given,
        JsonElement[] steps,
        QueueHarness harness
    )
    {
        if (given.TryGetProperty("validTickets", out var tickets))
        {
            foreach (var ticket in tickets.EnumerateArray())
            {
                var node = ticket.GetProperty("node").GetString()!;
                var session = ticket.GetProperty("sessionRid").GetString()!;
                harness.SeedTicket(node, session);
            }
        }
        if (given.TryGetProperty("runningPermits", out var permits))
        {
            foreach (var permit in permits.EnumerateArray())
                harness.SeedPermit(
                    permit.GetProperty("node").GetString()!,
                    Read(permit, "permit") ?? "running",
                    permit.TryGetProperty("requestSeq", out var seq) ? seq.GetUInt64() : 1,
                    Read(permit, "ownerId") ?? "o1",
                    permit.GetProperty("leaseGeneration").GetUInt64()
                );
        }
        if (given.TryGetProperty("waiting", out var waiters))
            foreach (var waiter in waiters.EnumerateArray())
                harness.Enqueue(
                    waiter.GetProperty("node").GetString()!,
                    waiter.GetProperty("sessionRid").GetString()!
                );

        var leaseChecks = 0;
        if (given.TryGetProperty("ownerLeases", out var leases))
        {
            harness.ReclaimExpiredOwners = (owners, _) =>
            {
                foreach (var owner in owners)
                {
                    leaseChecks++;
                    if (Read(leases, $"{owner.OwnerId}:{owner.Generation}") != "ended")
                        continue;
                    harness.Queue.ReclaimOwner(owner);
                }
                return ValueTask.CompletedTask;
            };
        }

        foreach (var step in steps)
        {
            var node = Read(step, "node") ?? "n1";
            var session = Read(step, "sessionRid") ?? "s2";
            var expected = step.GetProperty("expect");
            switch (step.GetProperty("action").GetString())
            {
                case "admitPermit":
                {
                    var result = harness.Admit(
                        node,
                        session,
                        step.GetProperty("requestSeq").GetUInt64(),
                        step.GetProperty("epoch").GetUInt64(),
                        step.GetProperty("ownerId").GetString()!,
                        step.GetProperty("leaseGeneration").GetUInt64(),
                        Read(step, "permit") ?? "last"
                    );
                    var granted = expected.GetProperty("granted").GetBoolean();
                    Assert.Equal(granted, result.Granted);
                    if (granted)
                        Assert.True(result.PermitId is > 0);
                    break;
                }
                case "releasePermit":
                    harness.Release(node, step.GetProperty("permit").GetString()!);
                    break;
                case "ownerLeaseEnded":
                {
                    var owner = new AdmissionOwnerLease(
                        step.GetProperty("ownerId").GetString()!,
                        step.GetProperty("leaseGeneration").GetUInt64()
                    );
                    var request = new AdmissionPermitRequest(
                        Rid("n2"),
                        1,
                        "o2",
                        5,
                        "login",
                        Rid("s2"),
                        42,
                        1
                    );
                    Assert.Equal(owner, Assert.Single(harness.Queue.PermitOwnersAtLimit(request)));
                    harness.Queue.ReclaimOwner(owner);
                    Assert.Empty(harness.Queue.PermitOwnersAtLimit(request));
                    break;
                }
                case "nodeReport":
                {
                    var report = harness.Report(
                        node,
                        limit: ReportLimit(given, node),
                        held: step.GetProperty("heldPermits")
                            .EnumerateArray()
                            .Select(item => harness.Permits[item.GetString()!])
                            .ToArray(),
                        resolved: step.GetProperty("resolvedRequestSeq").GetUInt64()
                    );
                    if (expected.TryGetProperty("ownerLeaseChecks", out var expectedChecks))
                        Assert.Equal(expectedChecks.GetInt32(), leaseChecks);
                    if (expected.TryGetProperty("ticketSessions", out var sessions))
                        Assert.Equal(
                            sessions.EnumerateArray().Select(item => Rid(item.GetString()!)),
                            report
                                .Deliveries.Where(item => item.Kind == "ticket")
                                .Select(item => item.SessionRid)
                        );
                    break;
                }
                case "enqueue":
                {
                    var delivery = harness.Enqueue(node, session);
                    Assert.Null(delivery.Ticket);
                    Assert.Equal(expected.GetProperty("position").GetInt32(), delivery.Position);
                    break;
                }
                case "nodeWaiters":
                {
                    var order = expected
                        .GetProperty("order")
                        .EnumerateArray()
                        .Select(item => item.GetString()!)
                        .ToArray();
                    Assert.Equal(
                        new int?[] { 1, 2 },
                        order.Select(item => harness.Enqueue(node, item).Position)
                    );
                    break;
                }
                case "issue":
                    Assert.Equal(
                        expected
                            .GetProperty("ticketSessions")
                            .EnumerateArray()
                            .Select(item => Rid(item.GetString()!)),
                        harness
                            .Report("n2", limit: 1)
                            .Deliveries.Where(item => item.Kind == "ticket")
                            .Select(item => item.SessionRid)
                    );
                    break;
                default:
                    throw new InvalidOperationException(step.GetRawText());
            }

            if (expected.TryGetProperty("runningPermits", out var count))
                Assert.Equal(count.GetInt32(), harness.Queue.RunningPermitCount);
        }
    }

    [Fact]
    public void ReportChecksLeasesOnlyForAWaiterWithLocalBudgetBlockedByTheGlobalLimit()
    {
        var harness = new QueueHarness(1);
        harness.SeedPermit("n1", "p1", 1);
        var checks = 0;
        harness.ReclaimExpiredOwners = (owners, _) =>
        {
            Assert.Equal(new AdmissionOwnerLease("o1", 3), Assert.Single(owners));
            checks++;
            return ValueTask.CompletedTask;
        };

        harness.Report("n2", limit: 1);
        Assert.Equal(0, checks);

        harness.Report("n2", limit: 1, inFlight: 1);
        harness.Enqueue("n2", "s2");
        harness.Report("n2", limit: 1, inFlight: 1);
        Assert.Equal(0, checks);

        harness.Report("n2", limit: 1);
        Assert.Equal(1, checks);
        Assert.Equal(1, harness.Queue.RunningPermitCount);
    }

    public static IEnumerable<object[]> Scenarios() =>
        AdmissionQueueFixture.Read(AdmissionQueueFixture.Queue, "queueScenarios");

    private static int ReportLimit(JsonElement given, string node)
    {
        if (given.TryGetProperty("reports", out var reports))
            foreach (var report in reports.EnumerateArray())
                if (Read(report, "node") == node)
                    return report.GetProperty("limit").GetInt32();
        return 0;
    }

    private static JsonElement Expected(JsonElement step, string field) =>
        step.GetProperty("expect").GetProperty(field);

    private static string? Read(JsonElement item, string property) =>
        item.TryGetProperty(property, out var value) ? value.GetString() : null;

    private static string Rid(string value) =>
        ZLinkAdmissionTicket.Encode(Encoding.UTF8.GetBytes(value));

    private sealed class QueueHarness
    {
        internal readonly ControllableTimeProvider Time = new();
        internal readonly ZLinkAdmissionTicket Ticket = new(AdmissionQueueFixture.Key, null);
        internal readonly Dictionary<string, ulong> Permits = [];
        internal readonly ZLinkAdmissionQueue Queue;
        private readonly ulong _epoch;

        internal QueueHarness(int? limit, ulong epoch = 42)
        {
            _epoch = epoch;
            Queue = new ZLinkAdmissionQueue(
                new ZLinkAdmissionQueueConfiguration("admission", limit, Ticket),
                Time,
                epoch
            );
        }

        internal AdmissionDelivery Enqueue(string node, string session, ulong generation = 1) =>
            Queue
                .Enqueue(
                    new AdmissionEnqueue(
                        new AdmissionConnection(Rid(node), generation, "login", Rid(session))
                    )
                )
                .Delivery;

        internal AdmissionReported Report(
            string node,
            int limit = 0,
            int inFlight = 0,
            string[]? used = null,
            string[]? ended = null,
            ulong generation = 1,
            ulong[]? held = null,
            ulong resolved = 0
        ) =>
            Queue
                .ReportAsync(
                    new AdmissionReport(
                        Rid(node),
                        generation,
                        Math.Max(limit, 1),
                        limit == 0 ? 1 : inFlight,
                        Sessions(used),
                        Sessions(ended),
                        held ?? [],
                        resolved
                    ),
                    ReclaimExpiredOwners,
                    CancellationToken.None
                )
                .GetAwaiter()
                .GetResult();

        internal Func<
            AdmissionOwnerLease[],
            CancellationToken,
            ValueTask
        > ReclaimExpiredOwners { get; set; } = static (_, _) => ValueTask.CompletedTask;

        internal void SeedTicket(string node, string session, bool use = true)
        {
            Report(node, limit: 1);
            Assert.Equal("ticket", Enqueue(node, session).Kind);
            if (use)
                Report(node, used: [session]);
        }

        internal void SeedPermit(
            string node,
            string label,
            ulong seq,
            string owner = "o1",
            ulong lease = 3
        )
        {
            SeedTicket(node, label);
            Assert.True(Admit(node, label, seq, _epoch, owner, lease, label).Granted);
        }

        internal AdmissionPermitResult Admit(
            string node,
            string session,
            ulong seq,
            ulong epoch,
            string owner,
            ulong lease,
            string label
        )
        {
            var result = Queue.Admit(
                new AdmissionPermitRequest(
                    Rid(node),
                    1,
                    owner,
                    lease,
                    "login",
                    Rid(session),
                    epoch,
                    seq
                )
            );
            if (result.PermitId is { } id)
                Permits[label] = id;
            return result;
        }

        internal void Release(string node, string label) =>
            Queue.Release(new AdmissionPermitRelease(Rid(node), 1, Permits[label]));

        private static AdmissionSession[] Sessions(string[]? values) =>
            values?.Select(value => new AdmissionSession("login", Rid(value))).ToArray() ?? [];
    }
}
