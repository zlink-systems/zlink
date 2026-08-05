namespace ZoneWorld.Shared.Contracts;

/// <summary>
/// The framework element names fixed by scenario §4. The name choices carry the
/// topology decision, so they live next to the wire contract rather than in each
/// server's configuration.
/// </summary>
public static class ZoneWorldNames
{
    /// <summary>Physical RouteMesh hosting all ZoneWorld MeshNodes.</summary>
    public const string MeshName = "zoneworld.mesh";

    /// <summary>Logical multicast channel containing the zone Spot owners.</summary>
    public const string ZoneChannel = "zoneworld.zones";

    /// <summary>Fanout channel: Ops publishes, every ZoneNode subscribes.</summary>
    public const string BroadcastChannel = "zoneworld.broadcast";

    /// <summary>RouteMesh ChannelName: ZoneNode reports to the ready Ops member.</summary>
    public const string ReportChannel = "zoneworld.report";

    public const string AnnounceTopic = "world.announce";
    public const string MaintenanceTopic = "world.maintenance";

    /// <summary>Zone spots publish the border band per adjacent zone (§4.1).</summary>
    public static string BorderTopic(string fromZoneId, string toZoneId) =>
        $"zone.border.{fromZoneId}.{toZoneId}";

    public const string NorthWestToNorthEastBorder = "zone.border.zone-nw.zone-ne";
    public const string NorthWestToSouthWestBorder = "zone.border.zone-nw.zone-sw";
    public const string NorthEastToNorthWestBorder = "zone.border.zone-ne.zone-nw";
    public const string NorthEastToSouthEastBorder = "zone.border.zone-ne.zone-se";
    public const string SouthWestToNorthWestBorder = "zone.border.zone-sw.zone-nw";
    public const string SouthWestToSouthEastBorder = "zone.border.zone-sw.zone-se";
    public const string SouthEastToNorthEastBorder = "zone.border.zone-se.zone-ne";
    public const string SouthEastToSouthWestBorder = "zone.border.zone-se.zone-sw";

    public const string PlayerActorType = "zoneworld.player";
    public const string ZoneSpotType = "zoneworld.zone";

    public const string GatewayStreamNode = "zoneworld.gateway";
    public const string OpsStreamNode = "zoneworld.ops";

    /// <summary>
    /// Monitoring source name used by the location runtime projection (§8.1).
    /// </summary>
    public const string OpsLocationSource = "zoneworld.ops.location";
}

public static class HandlerGroups
{
    public const string ZoneOps = "zone-ops";
    public const string ZoneBroadcast = "zone-broadcast";
    public const string BroadcastProbe = "broadcast-probe";
    public const string Ops = "ops";
}
