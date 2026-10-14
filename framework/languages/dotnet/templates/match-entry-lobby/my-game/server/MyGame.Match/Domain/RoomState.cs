using MyGame.Shared;
namespace MyGame.Match.Domain;

public sealed class RoomState
{
    private readonly record struct Wall(double X, double MinY, double MaxY);
    private static readonly Wall[] Walls = [new(GameNames.WallX, GameNames.WallMinY, GameNames.WallMaxY)];
    public List<Combatant> Players { get; set; } = [];
    public string? Winner { get; set; }
    public bool Admit(string playerId)
    {
        if (Players.Any(p => p.PlayerId == playerId)) return true;
        if (Players.Count >= GameNames.PlayersPerRoom) return false;
        Players.Add(new Combatant { PlayerId = playerId });
        return true;
    }
    private Combatant Player(string id) => Players.Single(p => p.PlayerId == id);
    private string? ValidateAction(Combatant player, long sequence)
    {
        if (sequence <= player.Sequence) return Rejection.StaleSequence;
        player.Sequence = sequence;
        return Winner is null ? null : Rejection.MatchFinished;
    }
    public string? Move(string id, MoveRequest move)
    {
        var player = Player(id);
        var reason = ValidateAction(player, move.Sequence);
        if (reason is not null) return reason;
        if (!double.IsFinite(move.X) || !double.IsFinite(move.Y)) return Rejection.InvalidMove;
        player.X = move.X;
        player.Y = move.Y;
        return null;
    }
    public string? Shoot(string id, ShootRequest shot)
    {
        var shooter = Player(id);
        var reason = ValidateAction(shooter, shot.Sequence);
        if (reason is not null) return reason;
        var target = Players.Find(p => p.PlayerId == shot.TargetId && p.PlayerId != id && p.Hp > 0);
        if (target is null) return Rejection.NoTarget;
        var dx = target.X - shooter.X;
        var dy = target.Y - shooter.Y;
        if (dx * dx + dy * dy > GameNames.ShotRange * GameNames.ShotRange) return Rejection.OutOfRange;
        if (Walls.Any(wall => IntersectsWall(shooter, target, wall))) return Rejection.BlockedByWall;
        target.Hp = Math.Max(0, target.Hp - GameNames.ShotDamage);
        if (target.Hp == 0) Winner = id;
        return null;
    }
    // Touching the closed wall segment also blocks the shot.
    private static bool IntersectsWall(Combatant a, Combatant b, Wall wall)
    {
        if (a.X == b.X)
            return a.X == wall.X && Math.Max(a.Y, b.Y) >= wall.MinY
                && Math.Min(a.Y, b.Y) <= wall.MaxY;
        var t = (wall.X - a.X) / (b.X - a.X);
        var y = a.Y + t * (b.Y - a.Y);
        return t >= 0 && t <= 1 && y >= wall.MinY && y <= wall.MaxY;
    }
    public PlayerView[] View() => Players.Select(p => new PlayerView(p.PlayerId, p.X, p.Y, p.Hp)).ToArray();
}
public sealed class Combatant
{
    public string PlayerId { get; set; } = "";
    public double X { get; set; }
    public double Y { get; set; }
    public int Hp { get; set; } = GameNames.InitialHp;
    public long Sequence { get; set; }
}
