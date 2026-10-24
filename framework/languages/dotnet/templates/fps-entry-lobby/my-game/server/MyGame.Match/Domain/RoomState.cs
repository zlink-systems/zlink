using System.Diagnostics;
using MyGame.Shared;

namespace MyGame.Match.Domain;

public sealed class RoomState
{
    private readonly Func<double> nowMs;
    public List<Combatant> Players { get; } = [];
    public MatchPhase Phase { get; private set; } = MatchPhase.Waiting;
    public string? Winner { get; private set; }

    // Only the domain constructor accepts a clock; production uses monotonic elapsed time.
    public RoomState(Func<double>? clock = null) =>
        nowMs = clock ?? (() => Stopwatch.GetTimestamp() * 1000.0 / Stopwatch.Frequency);

    public bool Admit(string playerId)
    {
        if (Players.Any(p => p.PlayerId == playerId))
            return true;
        if (Players.Count >= GameNames.PlayersPerRoom)
            return false;
        Players.Add(new Combatant(playerId, ArenaMap.Spawns[Players.Count]));
        if (Players.Count == GameNames.PlayersPerRoom)
            Phase = MatchPhase.Playing;
        return true;
    }

    private Combatant Player(string id) => Players.Single(p => p.PlayerId == id);

    private string? ValidateAction(Combatant player, long sequence) =>
        sequence <= player.Sequence ? Rejection.StaleSequence
        : Phase == MatchPhase.Finished ? Rejection.MatchFinished : null;

    public ActionRejected? Move(string id, MoveRequest move)
    {
        var player = Player(id);
        if (ValidateAction(player, move.Sequence) is { } reason)
            return new(reason);
        if (!move.Delta.IsFinite)
            return new(Rejection.NonFinite);
        if (move.Delta.Y != 0)
            return new(Rejection.VerticalMove);
        var distance = move.Delta.Length;
        if (distance > GameNames.MaxStep)
            return new(Rejection.StepTooLarge);
        var next = player.Position + move.Delta;
        if (!ArenaMap.Bounds.Contains(next))
            return new(Rejection.OutsideMap);
        if (ArenaMap.Walls.Any(wall => wall.Intersect(player.Position, move.Delta, 1) is not null))
            return new(Rejection.MoveBlockedByWall);
        var now = nowMs();
        if (player.LastMoveMs is { } lastMove)
        {
            var elapsed = Math.Max(0, now - lastMove);
            var budget = Math.Min(GameNames.MaxStep, GameNames.MaxSpeed * elapsed / 1000);
            if (distance > budget)
                return new(Rejection.MoveBudget,
                    Math.Max(1, (int)Math.Ceiling(distance * 1000 / GameNames.MaxSpeed - elapsed)));
        }
        player.Position = next;
        player.Sequence = move.Sequence;
        player.LastMoveMs = now;
        return null;
    }

    public ShotDecision Shoot(string id, ShootRequest shot)
    {
        var shooter = Player(id);
        if (ValidateAction(shooter, shot.Sequence) is { } reason)
            return new(new ShotRejected(reason), null);
        if (!shot.AimDirection.TryNormalize(out var direction))
            return new(new ShotRejected(Rejection.InvalidAim), null);
        var now = nowMs();
        if (shooter.NextFireMs is { } nextFire && now < nextFire)
            return new(new ShotRejected(Rejection.FireTooSoon,
                Math.Max(1, (int)Math.Ceiling(nextFire - now))), null);
        var origin = shooter.Position + new Vec3(0, GameNames.EyeHeight, 0);
        var wallDistance = double.PositiveInfinity;
        foreach (var wall in ArenaMap.Walls)
            if (wall.Intersect(origin, direction, GameNames.ShotRange) is { } distance)
                wallDistance = Math.Min(wallDistance, distance);
        Combatant? target = null;
        var targetDistance = double.PositiveInfinity;
        foreach (var candidate in Players)
        {
            if (candidate == shooter || candidate.Hp == 0)
                continue;
            if (ArenaMap.PlayerBox(candidate.Position).Intersect(origin, direction,
                    GameNames.ShotRange) is { } distance && distance < targetDistance)
            {
                target = candidate;
                targetDistance = distance;
            }
        }
        var outcome = ShotOutcome.Miss;
        if (wallDistance <= targetDistance && double.IsFinite(wallDistance))
            outcome = ShotOutcome.BlockedByWall;
        else if (target is not null)
        {
            outcome = ShotOutcome.Hit;
            target.Hp = Math.Max(0, target.Hp - GameNames.ShotDamage);
            if (target.Hp == 0)
            {
                Phase = MatchPhase.Finished;
                Winner = id;
            }
        }
        shooter.Sequence = shot.Sequence;
        shooter.NextFireMs = now + GameNames.FireIntervalMs;
        return new(null, new ShotResult(outcome, outcome == ShotOutcome.Hit ? target!.PlayerId : null,
            shot.Sequence));
    }

    public PlayerView[] View() => Players.Select(p =>
        new PlayerView(p.PlayerId, p.Position, p.Hp, p.Sequence)).ToArray();

    public RoomSnapshot Capture()
    {
        var now = nowMs();
        return new(Players.Select(p => new CombatantSnapshot(p.PlayerId, p.Position, p.Hp, p.Sequence,
            p.NextFireMs is { } fire ? Math.Max(0, fire - now) : 0,
            p.LastMoveMs is { } moved
                ? Math.Max(0, GameNames.MaxStep * 1000 / GameNames.MaxSpeed - (now - moved)) : 0))
            .ToArray(), Phase, Winner);
    }

    public void Restore(RoomSnapshot snapshot)
    {
        var now = nowMs();
        Players.Clear();
        foreach (var p in snapshot.Players)
            Players.Add(new Combatant(p.PlayerId, p.Position) {
                Hp = p.Hp, Sequence = p.Sequence, NextFireMs = now + p.FireRemainingMs,
                LastMoveMs = p.MoveRemainingMs > 0
                    ? now - (GameNames.MaxStep * 1000 / GameNames.MaxSpeed - p.MoveRemainingMs) : null,
            });
        Phase = snapshot.Phase;
        Winner = snapshot.Winner;
    }
}

public sealed class Combatant(string playerId, Vec3 position)
{
    public string PlayerId { get; } = playerId;
    public Vec3 Position { get; internal set; } = position;
    public int Hp { get; internal set; } = GameNames.InitialHp;
    public long Sequence { get; internal set; }
    internal double? LastMoveMs { get; set; }
    internal double? NextFireMs { get; set; }
}

public sealed record ShotDecision(ShotRejected? Rejected, ShotResult? Result);
public sealed record CombatantSnapshot(string PlayerId, Vec3 Position, int Hp, long Sequence,
    double FireRemainingMs, double MoveRemainingMs);
public sealed record RoomSnapshot(CombatantSnapshot[] Players, MatchPhase Phase, string? Winner);
