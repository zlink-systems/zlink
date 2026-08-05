using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;

namespace ZoneWorld.Server.ZoneNode.Application.Zone;

/// <summary>
/// Binds the movement rules to this node's view of maintenance. The domain knows the
/// coordinate rules but nothing about nodes, so the "may I enter that zone" question is
/// answered here (§2.2, §2.3).
/// </summary>
public sealed class MoveUseCase
{
    public MoveDecision Decide(PlayerPosition from, int toX, int toY) =>
        MovePolicy.Validate(from, toX, toY);
}
