using System.Text.Json;
using MyGame.Shared;

namespace MyGame.Match.Domain;

public sealed class RoomState
{
    private readonly record struct Wall(double X, double MinY, double MaxY);

    private sealed record RoomSnapshot(
        string Phase,
        string? Winner,
        CombatantSnapshot[] Players
    );

    private sealed record CombatantSnapshot(
        string PlayerId,
        double X,
        double Y,
        int Hp,
        long LastSequence,
        long? MoveBudgetRemainingMs,
        long? FireCooldownRemainingMs
    );

    private static readonly Wall[] Walls =
    [
        new(GameNames.WallX, GameNames.WallMinY, GameNames.WallMaxY),
    ];

    private static readonly TimeSpan MoveBudgetDuration = TimeSpan.FromSeconds(
        GameNames.MaxStep / GameNames.MaxSpeed
    );
    private readonly TimeProvider clock;
    private readonly List<Combatant> players = [];

    public RoomState(TimeProvider? clock = null)
    {
        this.clock = clock ?? TimeProvider.System;
    }

    public IReadOnlyList<Combatant> Players => players;
    public string? Winner { get; private set; }
    public string Phase => Winner is null ? GameNames.PlayingPhase : GameNames.FinishedPhase;

    public bool Admit(string playerId)
    {
        if (players.Any(player => player.PlayerId == playerId))
            return true;
        if (players.Count >= GameNames.PlayersPerRoom)
            return false;

        var spawn = players.Count == 0
            ? (GameNames.FirstSpawnX, GameNames.FirstSpawnY)
            : (GameNames.SecondSpawnX, GameNames.SecondSpawnY);
        players.Add(new Combatant(playerId, spawn.Item1, spawn.Item2));
        return true;
    }

    private Combatant Player(string id) => players.Single(player => player.PlayerId == id);

    private ActionRejected? ValidateAction(Combatant player, long sequence)
    {
        if (sequence > GameNames.MaxSafeInteger)
            return new ActionRejected(Rejection.IntegerOutOfRange);
        if (sequence <= player.LastSequence)
            return new ActionRejected(Rejection.StaleSequence);
        return Winner is null ? null : new ActionRejected(Rejection.MatchFinished);
    }

    private static void CommitAcceptedAction(Combatant player, long sequence) =>
        player.LastSequence = sequence;

    public ActionRejected? Move(string id, MoveRequest move)
    {
        var player = Player(id);
        var rejection = ValidateAction(player, move.Sequence);
        if (rejection is not null)
            return rejection;
        if (!double.IsFinite(move.Dx) || !double.IsFinite(move.Dy))
            return new ActionRejected(Rejection.InvalidMove);

        var distance = Math.Sqrt(move.Dx * move.Dx + move.Dy * move.Dy);
        if (distance > GameNames.MaxStep)
            return new ActionRejected(Rejection.StepTooLarge);

        var targetX = player.X + move.Dx;
        var targetY = player.Y + move.Dy;
        if (
            targetX < GameNames.MapMinX
            || targetX > GameNames.MapMaxX
            || targetY < GameNames.MapMinY
            || targetY > GameNames.MapMaxY
        )
            return new ActionRejected(Rejection.OutsideMap);

        if (Walls.Any(wall => IntersectsWall(player.X, player.Y, targetX, targetY, wall)))
            return new ActionRejected(Rejection.MoveBlockedByWall);

        var now = clock.GetTimestamp();
        if (player.LastMoveTimestamp is { } lastMove)
        {
            var elapsedMs = ElapsedMilliseconds(lastMove, now);
            var budget = Math.Min(GameNames.MaxStep, GameNames.MaxSpeed * elapsedMs / 1000.0);
            if (distance > budget)
            {
                var remainingDistance = distance - budget;
                var retryAfterMs = RetryAfterMilliseconds(
                    remainingDistance * 1000.0 / GameNames.MaxSpeed
                );
                return new ActionRejected(Rejection.MoveBudget, retryAfterMs);
            }
        }

        player.X = targetX;
        player.Y = targetY;
        CommitAcceptedAction(player, move.Sequence);
        player.LastMoveTimestamp = now;
        return null;
    }

    public ActionRejected? Shoot(string id, ShootRequest shot)
    {
        var shooter = Player(id);
        var rejection = ValidateAction(shooter, shot.Sequence);
        if (rejection is not null)
            return rejection;

        var target = players.Find(player => player.PlayerId == shot.TargetId && player.PlayerId != id && player.Hp > 0);
        if (target is null)
            return new ActionRejected(Rejection.NoTarget);

        var dx = target.X - shooter.X;
        var dy = target.Y - shooter.Y;
        if (dx * dx + dy * dy > GameNames.ShotRange * GameNames.ShotRange)
            return new ActionRejected(Rejection.OutOfRange);
        if (Walls.Any(wall => IntersectsWall(shooter.X, shooter.Y, target.X, target.Y, wall)))
            return new ActionRejected(Rejection.BlockedByWall);

        var now = clock.GetTimestamp();
        if (shooter.LastShotTimestamp is { } lastShot)
        {
            var retryAfterMs = GameNames.FireIntervalMs - ElapsedMilliseconds(lastShot, now);
            if (retryAfterMs > 0)
                return new ActionRejected(Rejection.FireTooSoon, RetryAfterMilliseconds(retryAfterMs));
        }

        target.Hp = Math.Max(0, target.Hp - GameNames.ShotDamage);
        if (target.Hp == 0)
            Winner = id;
        CommitAcceptedAction(shooter, shot.Sequence);
        shooter.LastShotTimestamp = now;
        return null;
    }

    private double ElapsedMilliseconds(long startingTimestamp, long endingTimestamp) =>
        Math.Max(0, clock.GetElapsedTime(startingTimestamp, endingTimestamp).TotalMilliseconds);

    private static int RetryAfterMilliseconds(double milliseconds)
    {
        if (double.IsNaN(milliseconds) || milliseconds <= 1)
            return 1;
        if (milliseconds >= int.MaxValue)
            return int.MaxValue;
        return (int)Math.Ceiling(milliseconds);
    }

    private static bool IntersectsWall(double ax, double ay, double bx, double by, Wall wall)
    {
        if (ax == bx)
            return ax == wall.X
                && Math.Max(ay, by) >= wall.MinY
                && Math.Min(ay, by) <= wall.MaxY;

        var t = (wall.X - ax) / (bx - ax);
        var y = ay + t * (by - ay);
        return t >= 0 && t <= 1 && y >= wall.MinY && y <= wall.MaxY;
    }

    public PlayerView[] View() =>
        players.Select(player => new PlayerView(player.PlayerId, player.X, player.Y, player.Hp, player.LastSequence)).ToArray();

    public byte[] Capture()
    {
        var now = clock.GetTimestamp();
        var snapshot = new RoomSnapshot(
            Phase,
            Winner,
            players.Select(player => new CombatantSnapshot(
                player.PlayerId,
                player.X,
                player.Y,
                player.Hp,
                player.LastSequence,
                RemainingMilliseconds(player.LastMoveTimestamp, MoveBudgetDuration, now),
                RemainingMilliseconds(
                    player.LastShotTimestamp,
                    TimeSpan.FromMilliseconds(GameNames.FireIntervalMs),
                    now
                )
            )).ToArray()
        );
        return JsonSerializer.SerializeToUtf8Bytes(snapshot);
    }

    public void Restore(ReadOnlyMemory<byte> payload)
    {
        var snapshot = JsonSerializer.Deserialize<RoomSnapshot>(payload.Span)
            ?? throw new InvalidDataException("Missing room state.");
        var expectedPhase = snapshot.Winner is null ? GameNames.PlayingPhase : GameNames.FinishedPhase;
        if (snapshot.Phase != expectedPhase || snapshot.Players.Length > GameNames.PlayersPerRoom)
            throw new InvalidDataException("Invalid room state snapshot.");

        var now = clock.GetTimestamp();
        players.Clear();
        foreach (var player in snapshot.Players)
        {
            players.Add(new Combatant(player.PlayerId, player.X, player.Y)
            {
                Hp = player.Hp,
                LastSequence = player.LastSequence,
                LastMoveTimestamp = RestoreTimestamp(
                    player.MoveBudgetRemainingMs,
                    MoveBudgetDuration,
                    now
                ),
                LastShotTimestamp = RestoreTimestamp(
                    player.FireCooldownRemainingMs,
                    TimeSpan.FromMilliseconds(GameNames.FireIntervalMs),
                    now
                ),
            });
        }
        Winner = snapshot.Winner;
    }

    private long? RemainingMilliseconds(long? lastTimestamp, TimeSpan duration, long now)
    {
        if (lastTimestamp is not { } timestamp)
            return null;
        return Math.Max(
            0,
            (long)Math.Ceiling(duration.TotalMilliseconds - ElapsedMilliseconds(timestamp, now))
        );
    }

    private long? RestoreTimestamp(long? remainingMilliseconds, TimeSpan duration, long now)
    {
        if (remainingMilliseconds is not { } remaining)
            return null;
        var boundedRemaining = Math.Clamp(remaining, 0, (long)duration.TotalMilliseconds);
        var elapsedMilliseconds = duration.TotalMilliseconds - boundedRemaining;
        var elapsedTimestamp = (long)Math.Ceiling(elapsedMilliseconds * clock.TimestampFrequency / 1000.0);
        return now - elapsedTimestamp;
    }
}

public sealed class Combatant(string playerId, double x, double y)
{
    public string PlayerId { get; } = playerId;
    public double X { get; internal set; } = x;
    public double Y { get; internal set; } = y;
    public int Hp { get; internal set; } = GameNames.InitialHp;
    public long LastSequence { get; internal set; }
    internal long? LastMoveTimestamp { get; set; }
    internal long? LastShotTimestamp { get; set; }
}
