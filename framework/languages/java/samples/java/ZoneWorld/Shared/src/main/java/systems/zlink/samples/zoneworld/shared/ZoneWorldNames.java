package systems.zlink.samples.zoneworld.shared;

public final class ZoneWorldNames {
    public static final String MESH = "zoneworld.mesh";
    public static final String ZONE_CHANNEL = "zoneworld.zones";
    public static final String BROADCAST_CHANNEL = "zoneworld.broadcast";
    public static final String REPORT_CHANNEL = "zoneworld.report";
    public static final String ANNOUNCE_TOPIC = "world.announce";
    public static final String MAINTENANCE_TOPIC = "world.maintenance";
    public static final String PLAYER_ACTOR_TYPE = "zoneworld.player";
    public static final String ZONE_SPOT_TYPE = "zoneworld.zone";
    public static final String GATEWAY_STREAM = "zoneworld.gateway";
    public static final String OPS_STREAM = "zoneworld.ops";
    public static final String OPS_HANDLER_GROUP = "zoneworld.ops.handlers";
    public static final String BROADCAST_HANDLER_GROUP = "zoneworld.broadcast.handlers";

    public static final String NW_NE = "zone.border.zone-nw.zone-ne";
    public static final String NW_SW = "zone.border.zone-nw.zone-sw";
    public static final String NE_NW = "zone.border.zone-ne.zone-nw";
    public static final String NE_SE = "zone.border.zone-ne.zone-se";
    public static final String SW_NW = "zone.border.zone-sw.zone-nw";
    public static final String SW_SE = "zone.border.zone-sw.zone-se";
    public static final String SE_NE = "zone.border.zone-se.zone-ne";
    public static final String SE_SW = "zone.border.zone-se.zone-sw";

    private ZoneWorldNames() {
    }

    public static String borderTopic(String from, String to) {
        return "zone.border." + from + "." + to;
    }
}
