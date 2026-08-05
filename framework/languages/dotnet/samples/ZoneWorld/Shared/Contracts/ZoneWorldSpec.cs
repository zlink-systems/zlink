namespace ZoneWorld.Shared.Contracts;

/// <summary>
/// The world rules from the ZoneWorld scenario §2. Every language server and the
/// browser client read these same values, so a change here changes all of them.
/// </summary>
public static class ZoneWorldSpec
{
    public const int WorldSize = 100;
    public const int ZoneSplit = 50;
    public const int BorderBand = 10;
    public const int MaxStepPerAxis = 5;
    public const int SpawnX = 25;
    public const int SpawnY = 25;

    /// <summary>
    /// Returns the zone that owns a coordinate. Placement, movement and entry all use this
    /// policy so the zone split cannot drift between server roles.
    /// </summary>
    public static string ZoneOf(int x, int y)
    {
        var west = x < ZoneSplit;
        var north = y < ZoneSplit;
        return (west, north) switch
        {
            (true, true) => ZoneIds.NorthWest,
            (false, true) => ZoneIds.NorthEast,
            (true, false) => ZoneIds.SouthWest,
            (false, false) => ZoneIds.SouthEast
        };
    }

    public const int TickPeriodMs = 100;
    public const int BotTickPeriodMs = 500;
    public const int BotStep = 3;

    /// <summary>Every zone runs one X patroller and one Y patroller, so the world holds eight
    /// bots in all (§2.7).</summary>
    public const int BotsPerZone = 2;

    public const int BotCount = BotsPerZone * 4;

    /// <summary>An adjacent-zone snapshot older than this many ticks is dropped (§2.4).</summary>
    public const int BorderSnapshotExpiryTicks = 3;

    /// <summary>Each ZoneNode reports its status to Ops on this period (§8.1).</summary>
    public const int NodeStatusReportPeriodMs = 1000;
}

public static class ZoneIds
{
    public const string NorthWest = "zone-nw";
    public const string NorthEast = "zone-ne";
    public const string SouthWest = "zone-sw";
    public const string SouthEast = "zone-se";

    public static readonly IReadOnlyList<string> All = [NorthWest, NorthEast, SouthWest, SouthEast];
}

/// <summary>
/// The bots' names (§2.7). They are part of the world, not of one server: the scenario client
/// names the bot it is watching, and the browser client draws them, so the roster cannot live
/// inside a server's policy class.
/// </summary>
public static class BotIds
{
    public const string NorthWestX = "bot-nw-x";
    public const string NorthWestY = "bot-nw-y";
    public const string NorthEastX = "bot-ne-x";
    public const string NorthEastY = "bot-ne-y";
    public const string SouthWestX = "bot-sw-x";
    public const string SouthWestY = "bot-sw-y";
    public const string SouthEastX = "bot-se-x";
    public const string SouthEastY = "bot-se-y";
}

public static class NodeIds
{
    public const string West = "zone-node-1";
    public const string East = "zone-node-2";

    /// <summary>Hosts no zone; registers only the fanout subscriber (§11.1, ZW-D2).</summary>
    public const string Extra = "zone-node-3";
}

/// <summary>The reason a move was rejected. The order of evaluation is fixed by §2.2.</summary>
public static class MoveRejectReasons
{
    public const string OutOfRange = "OutOfRange";
    public const string TooFar = "TooFar";
    public const string DiagonalCrossing = "DiagonalCrossing";
    public const string ZoneMaintenance = "ZoneMaintenance";
}

public static class NodeAlertKinds
{
    public const string TimerHandlerFailed = "TimerHandlerFailed";
}

public static class ZoneWorldErrors
{
    public const string NodeUnavailable = "NodeUnavailable";
}
