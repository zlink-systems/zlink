using MyGame.Shared;
using MyGame.Zone;
using System.Text.Json;

namespace MyGame.E2E;

internal static class DomainChecks
{
    private static int passed;
    private static int assertions;

    public static void Run()
    {
        passed = 0;
        assertions = 0;
        Check("non-finite movement", RejectsNonFiniteMovement);
        Check("step limit", RejectsOversizedStep);
        Check("world boundary", RejectsOutsideWorld);
        Check("movement budget", RejectsUntilServerRetry);
        Check("stale and unsafe sequence", RejectsInvalidSequence);
        Check("initial location resolution", ResolvesInitialLocation);
        Check("remaining-time relocation snapshot", RestoresRemainingBudget);
        Check("presence lifecycle", TracksPresenceLifecycle);
        Check("enum and nullable JSON", PreservesJsonContract);
        Console.WriteLine($"Domain checks: {passed}/9 passed; assertions: {assertions}");
    }

    private static void Check(string name, Action check)
    {
        check();
        Console.WriteLine($"PASS {name}");
        passed++;
    }

    private static void Require(bool condition, string message)
    {
        assertions++;
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static ZonePositionDomain Create(out ManualClock clock)
    {
        clock = new ManualClock();
        var domain = new ZonePositionDomain("zone-2", clock.ReadMilliseconds);
        domain.AddPlayer("player", new EnterZone(new Position(100), 0, 0));
        return domain;
    }

    private static void RejectsNonFiniteMovement()
    {
        var domain = Create(out _);
        foreach (var delta in new[] { double.NaN, double.PositiveInfinity, double.NegativeInfinity })
            RequireRejected(domain, delta, 1, "non-finite");
    }

    private static void RejectsOversizedStep()
    {
        var domain = Create(out _);
        RequireRejected(domain, ZonePositionDomain.MaxStep + 1, 1, "step-too-large");
    }

    private static void RejectsOutsideWorld()
    {
        var domain = new ZonePositionDomain("zone-6", () => 0);
        domain.AddPlayer("edge", new EnterZone(new Position(GameNames.WorldWidth - 1), 0, 0));
        RequireRejected(domain, 1, 1, "outside-world", "edge");
    }

    private static void RejectsUntilServerRetry()
    {
        var domain = Create(out var clock);
        var first = domain.Move("player", new MoveRequest(ZonePositionDomain.MaxStep, 1));
        Require(first.Accepted && first.Position.X == 110, "The initial move must have MaxStep budget.");
        var before = domain.State("player");
        var rejected = domain.Move("player", new MoveRequest(ZonePositionDomain.MaxStep, 2));
        var rejection = rejected.Rejection ?? throw new InvalidOperationException("Expected move-budget rejection.");
        Require(rejection.Reason == "move-budget", "Expected move-budget rejection.");
        Require(rejection.RetryAfterMs > 0, "move-budget must report a positive retry delay.");
        Require(domain.State("player") == before, "A rejected move changed position or sequence.");
        clock.Advance(TimeSpan.FromMilliseconds(rejection.RetryAfterMs));
        var accepted = domain.Move("player", new MoveRequest(ZonePositionDomain.MaxStep, 2));
        Require(accepted.Accepted && accepted.Position.X == 120, "The server retry delay was insufficient.");
    }

    private static void RejectsInvalidSequence()
    {
        var domain = Create(out _);
        RequireRejected(domain, 1, 0, "stale-sequence");
        RequireRejected(domain, 1, GameNames.MaxSafeInteger + 1, "integer-out-of-range");
        Require(GameNames.MaxSafeInteger == 9_007_199_254_740_991,
            "The contract integer limit changed.");
        Require(GameNames.IsSafeInteger((ulong)GameNames.MaxSafeInteger),
            "The maximum safe integer was rejected.");
        Require(!GameNames.IsSafeInteger((ulong)GameNames.MaxSafeInteger + 1),
            "An integer above the contract limit was accepted.");

        var gapDomain = Create(out _);
        var gap = gapDomain.Move("player", new MoveRequest(1, 2));
        Require(
            gap.Accepted && gap.Sequence == 2,
            "A new sequence greater than the last processed sequence was rejected."
        );

        var maxDomain = new ZonePositionDomain("zone-2", () => 0);
        maxDomain.AddPlayer(
            "last",
            new EnterZone(new Position(100), GameNames.MaxSafeInteger, 0)
        );
        RequireRejected(maxDomain, 1, GameNames.MaxSafeInteger, "stale-sequence", "last");
        RequireRejected(maxDomain, 1, long.MaxValue, "integer-out-of-range", "last");

        var safeArrival = new ZonePositionDomain("zone-2", () => 0);
        Require(
            safeArrival.QueueArrival(
                "safe",
                new EnterZone(new Position(100), GameNames.MaxSafeInteger, 0)
            ) is null,
            "The maximum safe sequence was rejected during Zone admission."
        );
        var unsafeArrival = new ZonePositionDomain("zone-2", () => 0);
        var arrivalRejection = unsafeArrival.QueueArrival(
            "unsafe",
            new EnterZone(new Position(100), GameNames.MaxSafeInteger + 1, 0)
        );
        Require(
            arrivalRejection?.Reason == "integer-out-of-range"
                && !unsafeArrival.TryTakeArrival("unsafe", out _),
            "An out-of-range admission sequence was accepted or stored."
        );
    }

    private static void ResolvesInitialLocation()
    {
        var valid = ZonePositionDomain.ResolveLocation(299);
        Require(valid.ZoneId == "zone-3" && valid.Rejection is null,
            "An in-world initial position resolved to the wrong Zone.");
        Require(ZonePositionDomain.ResolveLocation(double.NaN).Rejection?.Reason == "non-finite",
            "A non-finite initial position was not rejected.");
        Require(ZonePositionDomain.ResolveLocation(double.PositiveInfinity).Rejection?.Reason == "non-finite",
            "An infinite initial position was not rejected.");
        Require(ZonePositionDomain.ResolveLocation(GameNames.WorldWidth).Rejection?.Reason == "outside-world",
            "An initial position at the world limit was not rejected.");
    }

    private static void RestoresRemainingBudget()
    {
        var source = Create(out var sourceClock);
        var accepted = source.Move("player", new MoveRequest(ZonePositionDomain.MaxStep, 1));
        Require(accepted.Accepted, "Expected initial move to be accepted.");
        sourceClock.Advance(TimeSpan.FromMilliseconds(75));
        var snapshot = source.Capture();

        var destinationClock = new ManualClock(500);
        var destination = new ZonePositionDomain("zone-2", destinationClock.ReadMilliseconds);
        destination.Restore(snapshot);
        var rejected = destination.Move("player", new MoveRequest(5, 2));
        var rejection = rejected.Rejection ?? throw new InvalidOperationException("Expected move-budget rejection.");
        Require(rejection.Reason == "move-budget", "Relocation lost the remaining budget.");
        Require(rejection.RetryAfterMs == 25, "Relocation did not preserve remaining milliseconds.");
    }

    private static void TracksPresenceLifecycle()
    {
        var domain = Create(out _);
        domain.AddPlayer("observer", new EnterZone(new Position(101), 0, 0));
        Require(domain.ConnectedPlayers().Count == 2, "Connected players were not tracked by the Zone.");
        var before = domain.State("player");
        var disconnected = domain.SetConnected("player", false);
        Require(disconnected is { Connected: false }, "Disconnect did not end visibility.");
        Require(domain.ConnectedPlayers().Count == 1, "Disconnected players remained visible.");
        Require(domain.State("player").Position == before.Position, "Disconnect erased the stored position.");
        var reconnected = domain.SetConnected("player", true);
        Require(reconnected is { Connected: true }, "Reconnect did not restore visibility.");
        Require(domain.State("player").Position == before.Position, "Reconnect changed the stored position.");
        var left = domain.RemovePlayer("player");
        Require(left?.Position == before.Position, "Zone leave lost the last observed position.");
        Require(domain.ConnectedPlayers().Count == 1, "Zone leave retained the player.");
    }

    private static void PreservesJsonContract()
    {
        var options = new JsonSerializerOptions(JsonSerializerDefaults.Web);
        var left = JsonSerializer.Serialize(
            new PlayerLeft("player", PlayerLeftReason.LeftZone),
            options
        );
        Require(left.Contains("\"reason\":\"left-zone\"", StringComparison.Ordinal),
            "PlayerLeft reason was not serialized as its kebab-case name.");
        var border = JsonSerializer.Serialize(
            new BorderPresence(
                "zone-2",
                "player",
                new Position(10),
                1,
                BorderPresenceKind.Moved,
                null
            ),
            options
        );
        Require(border.Contains("\"kind\":\"moved\"", StringComparison.Ordinal),
            "Border event kind was not serialized as its kebab-case name.");
        Require(border.Contains("\"reason\":null", StringComparison.Ordinal),
            "A nullable border reason was omitted instead of encoded as null.");
        var rejected = JsonSerializer.Serialize(new ActionRejected("outside-world", 0), options);
        Require(rejected.Contains("\"reason\":\"outside-world\"", StringComparison.Ordinal),
            "ActionRejected reason field name changed.");
        Require(rejected.Contains("\"retryAfterMs\":0", StringComparison.Ordinal),
            "ActionRejected retryAfterMs field name changed.");
        Require(
            typeof(ActionRejected).GetProperty(nameof(ActionRejected.RetryAfterMs))?.PropertyType
                == typeof(int),
            "ActionRejected RetryAfterMs must use a 32-bit integer."
        );
    }

    private static void RequireRejected(
        ZonePositionDomain domain,
        double delta,
        long sequence,
        string reason,
        string playerId = "player"
    )
    {
        var before = domain.State(playerId);
        var result = domain.Move(playerId, new MoveRequest(delta, sequence));
        var rejection = result.Rejection ?? throw new InvalidOperationException($"Expected {reason} rejection.");
        Require(rejection.Reason == reason, $"Expected {reason}, received {rejection.Reason}.");
        Require(rejection.RetryAfterMs == 0, $"{reason} must not report a retry delay.");
        Require(domain.State(playerId) == before, $"Rejected {reason} changed domain state.");
    }

    private sealed class ManualClock(long initialMilliseconds = 0)
    {
        private long now = initialMilliseconds;
        public long ReadMilliseconds() => now;
        public void Advance(TimeSpan duration) => now += (long)duration.TotalMilliseconds;
    }
}
