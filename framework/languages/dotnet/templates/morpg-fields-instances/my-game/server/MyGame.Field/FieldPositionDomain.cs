using MyGame.Shared;

namespace MyGame.Field;

public sealed class FieldPositionDomain(string fieldId, Func<long>? monotonicMilliseconds = null)
{
    public const double MaxStep = 10;
    public const double MaxSpeed = MaxStep * 5;
    public const int FullBudgetAfterMs = 200;

    private readonly Func<long> clock = monotonicMilliseconds ?? ReadMonotonicMilliseconds;
    private readonly Dictionary<string, PlayerState> players = new(StringComparer.Ordinal);
    private readonly Dictionary<string, EnterField> arrivals = new(StringComparer.Ordinal);

    public static FieldLocationResolution ResolveLocation(double x)
    {
        if (!double.IsFinite(x))
            return new(null, new ActionRejected("non-finite", 0));
        if (x < 0 || x >= GameNames.FieldWidth * GameNames.FieldCount)
            return new(null, new ActionRejected("outside-world", 0));
        return new(GameNames.FieldId(x), null);
    }

    public ActionRejected? QueueArrival(string actorId, EnterField arrival)
    {
        var location = ResolveLocation(arrival.X);
        if (location.Rejection is not null)
            return location.Rejection;
        if (arrival.Sequence > GameNames.MaxSafeInteger)
            return new ActionRejected("integer-out-of-range", 0);
        if (arrival.Sequence < 0)
            return new ActionRejected("stale-sequence", 0);
        if (arrival.RemainingMs < 0 || arrival.RemainingMs > FullBudgetAfterMs)
            return new ActionRejected("integer-out-of-range", 0);
        if (location.FieldId != fieldId)
            return new ActionRejected("outside-world", 0);
        arrivals[actorId] = arrival;
        return null;
    }

    public bool TryTakeArrival(string actorId, out EnterField arrival) =>
        arrivals.Remove(actorId, out arrival!);

    public void AddPlayer(string actorId, EnterField arrival, bool connected = true)
    {
        var acceptedAt = clock() - (FullBudgetAfterMs - arrival.RemainingMs);
        players[actorId] = new PlayerState(
            new Position(arrival.X),
            arrival.Sequence,
            acceptedAt,
            connected
        );
    }

    public MovementResult Move(string actorId, MoveRequest request)
    {
        if (!players.TryGetValue(actorId, out var player))
            return new MovementResult(
                false,
                new Position(0),
                0,
                0,
                new ActionRejected("stale-sequence", 0),
                fieldId,
                false
            );
        if (!double.IsFinite(request.Delta))
            return Reject(actorId, "non-finite");
        if (Math.Abs(request.Delta) > MaxStep)
            return Reject(actorId, "step-too-large");
        if (request.Sequence > GameNames.MaxSafeInteger)
            return Reject(actorId, "integer-out-of-range");
        if (
            request.Sequence < 0
            || player.Sequence >= GameNames.MaxSafeInteger
            || request.Sequence <= player.Sequence
        )
            return Reject(actorId, "stale-sequence");

        var nextX = player.Position.X + request.Delta;
        var location = ResolveLocation(nextX);
        if (location.Rejection is not null)
            return Reject(actorId, location.Rejection.Reason);

        var now = clock();
        var elapsedMs = Math.Max(0, now - player.LastAcceptedMilliseconds);
        var budget = Math.Min(MaxStep, MaxSpeed * elapsedMs / 1000d);
        var distance = Math.Abs(request.Delta);
        if (distance > budget)
        {
            var retryAfterMs = (int)Math.Max(
                1,
                (long)Math.Ceiling((distance - budget) * 1000 / MaxSpeed)
            );
            return Reject(actorId, "move-budget", retryAfterMs);
        }

        var position = new Position(nextX);
        var nextFieldId = location.FieldId!;
        var relocates = nextFieldId != fieldId;
        if (!relocates)
        {
            players[actorId] = player with
            {
                Position = position,
                Sequence = request.Sequence,
                LastAcceptedMilliseconds = now,
            };
        }
        return new MovementResult(
            true,
            position,
            request.Sequence,
            FullBudgetAfterMs,
            null,
            nextFieldId,
            relocates
        );
    }

    public PlayerStateView State(string actorId)
    {
        var player = players[actorId];
        return new PlayerStateView(
            player.Position,
            player.Sequence,
            RemainingMs(player.LastAcceptedMilliseconds, clock()),
            player.Connected
        );
    }

    public PlayerStateView? RemovePlayer(string actorId)
    {
        if (!players.Remove(actorId, out var player))
            return null;
        return new PlayerStateView(
            player.Position,
            player.Sequence,
            RemainingMs(player.LastAcceptedMilliseconds, clock()),
            player.Connected
        );
    }

    public PlayerStateView? SetConnected(string actorId, bool connected)
    {
        if (!players.TryGetValue(actorId, out var player))
            return null;
        players[actorId] = player with { Connected = connected };
        return State(actorId);
    }

    public FieldPositionSnapshot Capture()
    {
        var now = clock();
        var snapshots = players.ToDictionary(
            pair => pair.Key,
            pair => new FieldPlayerSnapshot(
                pair.Value.Position,
                pair.Value.Sequence,
                RemainingMs(pair.Value.LastAcceptedMilliseconds, now),
                pair.Value.Connected
            ),
            StringComparer.Ordinal
        );
        return new FieldPositionSnapshot(fieldId, snapshots, new(arrivals, StringComparer.Ordinal));
    }

    public void Restore(FieldPositionSnapshot snapshot)
    {
        if (snapshot.FieldId != fieldId)
            throw new InvalidDataException("Field relocation state belongs to a different Field.");
        players.Clear();
        var now = clock();
        foreach (var (actorId, state) in snapshot.Players)
        {
            var acceptedAt = now - (FullBudgetAfterMs - state.RemainingMs);
            players.Add(
                actorId,
                new PlayerState(
                    state.Position,
                    state.Sequence,
                    acceptedAt,
                    state.Connected
                )
            );
        }
        arrivals.Clear();
        foreach (var (actorId, arrival) in snapshot.Arrivals)
            arrivals.Add(actorId, arrival);
    }

    private MovementResult Reject(string actorId, string reason, int retryAfterMs = 0)
    {
        var player = players[actorId];
        return new MovementResult(
            false,
            player.Position,
            player.Sequence,
            RemainingMs(player.LastAcceptedMilliseconds, clock()),
            new ActionRejected(reason, retryAfterMs),
            fieldId,
            false
        );
    }

    private static int RemainingMs(long acceptedAt, long now)
    {
        var remaining = FullBudgetAfterMs - Math.Max(0, now - acceptedAt);
        return (int)Math.Clamp(remaining, 0, FullBudgetAfterMs);
    }

    private static long ReadMonotonicMilliseconds() => Environment.TickCount64;

    private sealed record PlayerState(
        Position Position,
        long Sequence,
        long LastAcceptedMilliseconds,
        bool Connected
    );
}

public sealed record MovementResult(
    bool Accepted,
    Position Position,
    long Sequence,
    int RemainingMs,
    ActionRejected? Rejection,
    string FieldId,
    bool Relocates
);

public sealed record FieldLocationResolution(string? FieldId, ActionRejected? Rejection);

public sealed record PlayerStateView(
    Position Position,
    long Sequence,
    int RemainingMs,
    bool Connected
);

public sealed record FieldPlayerSnapshot(
    Position Position,
    long Sequence,
    int RemainingMs,
    bool Connected
);

public sealed record FieldPositionSnapshot(
    string FieldId,
    Dictionary<string, FieldPlayerSnapshot> Players,
    Dictionary<string, EnterField> Arrivals
);

public sealed record Position(double X);
