using MyGame.Shared;

namespace MyGame.Region.Domain;

public sealed record PositionSnapshot(string PlayerId, double X, long Sequence, long RemainingMs);

public sealed record PositionsSnapshot(PositionSnapshot[] Players);

public sealed class Positions(TimeProvider? clock = null)
{
    private sealed class PositionState(double x, long sequence, double budget, long updatedAt)
    {
        public double X { get; set; } = x;
        public long Sequence { get; set; } = sequence;
        public double Budget { get; set; } = budget;
        public long UpdatedAt { get; set; } = updatedAt;
    }

    private readonly TimeProvider clock = clock ?? TimeProvider.System;
    private readonly Dictionary<string, PositionState> players = new(StringComparer.Ordinal);

    public int Count => players.Count;

    public bool Contains(string playerId) => players.ContainsKey(playerId);

    public void Add(string playerId) =>
        players.TryAdd(
            playerId,
            new PositionState(GameNames.RegionMinX, 0, GameNames.MaxStep, clock.GetTimestamp())
        );

    public bool Remove(string playerId) => players.Remove(playerId);

    public ActionRejected? Move(string playerId, MoveRequest request)
    {
        var state = players[playerId];
        if (request.Sequence > GameNames.MaxSafeInteger)
            return new ActionRejected(Rejection.IntegerOutOfRange, 0);
        if (request.Sequence <= state.Sequence)
            return new ActionRejected(Rejection.StaleSequence, 0);
        if (!double.IsFinite(request.Delta))
            return new ActionRejected(Rejection.NonFinite, 0);

        var distance = Math.Abs(request.Delta);
        if (distance > GameNames.MaxStep)
            return new ActionRejected(Rejection.StepTooLarge, 0);

        var nextX = state.X + request.Delta;
        if (nextX < GameNames.RegionMinX || nextX > GameNames.RegionMaxX)
            return new ActionRejected(Rejection.OutsideRegion, 0);

        var now = clock.GetTimestamp();
        var available = AvailableBudget(state, now);
        if (distance > available)
        {
            var retryAfterMs = (int)
                Math.Ceiling((distance - available) / GameNames.MaxSpeed * 1000.0);
            return new ActionRejected(Rejection.MoveBudget, Math.Max(1, retryAfterMs));
        }

        state.X = nextX;
        state.Sequence = request.Sequence;
        state.Budget = available - distance;
        state.UpdatedAt = now;
        return null;
    }

    public PlayerPosition[] View() =>
        players
            .OrderBy(pair => pair.Key, StringComparer.Ordinal)
            .Select(pair => new PlayerPosition(pair.Key, pair.Value.X, pair.Value.Sequence))
            .ToArray();

    public PositionsSnapshot Capture()
    {
        var now = clock.GetTimestamp();
        return new PositionsSnapshot(
            players
                .OrderBy(pair => pair.Key, StringComparer.Ordinal)
                .Select(pair =>
                {
                    var available = AvailableBudget(pair.Value, now);
                    var remainingMs = (long)
                        Math.Ceiling((GameNames.MaxStep - available) / GameNames.MaxSpeed * 1000.0);
                    return new PositionSnapshot(
                        pair.Key,
                        pair.Value.X,
                        pair.Value.Sequence,
                        Math.Max(0, remainingMs)
                    );
                })
                .ToArray()
        );
    }

    public void Restore(PositionsSnapshot snapshot)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        ArgumentNullException.ThrowIfNull(snapshot.Players);
        var now = clock.GetTimestamp();
        var restored = new Dictionary<string, PositionState>(StringComparer.Ordinal);
        var maximumRemainingMs = (long)
            Math.Ceiling(GameNames.MaxStep / GameNames.MaxSpeed * 1000.0);
        foreach (var player in snapshot.Players)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(player.PlayerId);
            if (
                !double.IsFinite(player.X)
                || player.X < GameNames.RegionMinX
                || player.X > GameNames.RegionMaxX
                || Math.Abs(player.X) > GameNames.MaxSafeInteger
                || player.Sequence < 0
                || player.Sequence > GameNames.MaxSafeInteger
                || player.RemainingMs < 0
                || player.RemainingMs > maximumRemainingMs
            )
                throw new InvalidDataException("Invalid Region position snapshot.");
            var available = GameNames.MaxStep - player.RemainingMs * GameNames.MaxSpeed / 1000.0;
            if (!restored.TryAdd(
                    player.PlayerId,
                    new PositionState(player.X, player.Sequence, available, now)
                ))
                throw new InvalidDataException("Duplicate player in Region position snapshot.");
        }
        players.Clear();
        foreach (var pair in restored)
            players.Add(pair.Key, pair.Value);
    }

    private double AvailableBudget(PositionState state, long now)
    {
        var elapsedSeconds = clock.GetElapsedTime(state.UpdatedAt, now).TotalSeconds;
        if (elapsedSeconds <= 0)
            return state.Budget;
        return Math.Min(GameNames.MaxStep, state.Budget + elapsedSeconds * GameNames.MaxSpeed);
    }
}
