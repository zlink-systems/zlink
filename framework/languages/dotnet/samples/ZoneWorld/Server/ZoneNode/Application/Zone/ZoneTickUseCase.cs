using ZoneWorld.Server.ZoneNode.Domain.ZoneWorld;
using ZoneWorld.Shared.Contracts;

namespace ZoneWorld.Server.ZoneNode.Application.Zone;

/// <summary>Everything one tick produces. Pure data: the spot turns it into sends and
/// publishes, and a test can assert on it without a runtime.</summary>
public sealed record ZoneTickOutput(
    ZoneStateNotify Notify,
    IReadOnlyList<string> PushTargets,
    IReadOnlyList<ZoneBorderEvent> BorderEvents);

/// <summary>
/// One tick of a zone (§2.5): advance the counter, build the player list the clients see,
/// build the border snapshot for each adjacent zone, then drop the adjacent snapshots
/// that stopped arriving.
/// </summary>
public static class ZoneTickUseCase
{
    public static ZoneTickOutput Advance(ZoneState state)
    {
        var tick = state.NextTick();
        var notify = new ZoneStateNotify(state.ZoneId, tick, state.VisiblePlayers());

        var borderEvents = World.AdjacentZones(state.ZoneId)
            .Select(adjacent => new ZoneBorderEvent(
                state.ZoneId,
                adjacent,
                tick,
                state.BorderBandFor(adjacent)))
            .ToArray();

        // Bots are not push targets: they have no bound session, so a push addressed to
        // one would be a message with nowhere to go (§2.7, ZW-F3).
        var pushTargets = Humans(state);

        state.ExpireStaleSnapshots();

        return new ZoneTickOutput(notify, pushTargets, borderEvents);
    }

    public static IReadOnlyList<string> Humans(ZoneState state) =>
        Residents(state, isBot: false);

    public static IReadOnlyList<string> Bots(ZoneState state) =>
        Residents(state, isBot: true);

    private static IReadOnlyList<string> Residents(ZoneState state, bool isBot) =>
        state.ResidentIds
            .Where(playerId => state.TryGetResident(playerId, out var resident) && resident.IsBot == isBot)
            .ToArray();
}
