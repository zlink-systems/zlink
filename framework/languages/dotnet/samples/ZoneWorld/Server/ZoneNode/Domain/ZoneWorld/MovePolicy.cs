using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;

public abstract record MoveDecision
{
    private MoveDecision()
    {
    }

    /// <summary>The move stands. <paramref name="ZoneChanged"/> tells the caller whether the
    /// actor must join a different zone spot, which across nodes means relocation (§2.6).</summary>
    public sealed record Accepted(PlayerPosition To, bool ZoneChanged) : MoveDecision;

    public sealed record Rejected(string Reason) : MoveDecision;
}

/// <summary>
/// Movement validation (§2.2). The order of the checks is part of the contract: a move
/// may violate several rules at once, and every language must report the same one, so
/// the first rule that fires wins and the rest are not evaluated.
/// </summary>
public static class MovePolicy
{
    public static MoveDecision Validate(
        PlayerPosition from,
        int toX,
        int toY)
    {
        if (!World.InRange(toX, toY))
            return new MoveDecision.Rejected(MoveRejectReasons.OutOfRange);

        if (Math.Abs(toX - from.X) > ZoneWorldSpec.MaxStepPerAxis
            || Math.Abs(toY - from.Y) > ZoneWorldSpec.MaxStepPerAxis)
            return new MoveDecision.Rejected(MoveRejectReasons.TooFar);

        var to = new PlayerPosition(toX, toY);
        var crossesX = World.IsWest(from.ZoneId) != World.IsWest(to.ZoneId);
        var crossesY = World.IsNorth(from.ZoneId) != World.IsNorth(to.ZoneId);
        if (crossesX && crossesY)
            return new MoveDecision.Rejected(MoveRejectReasons.DiagonalCrossing);

        var zoneChanged = from.ZoneId != to.ZoneId;
        return new MoveDecision.Accepted(to, zoneChanged);
    }
}
