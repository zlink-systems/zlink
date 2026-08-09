package systems.zlink.samples.kotlin.zoneworld.shared

object ZoneWorldNames {
    const val MESH = "zoneworld.mesh"
    const val ZONE_CHANNEL = "zoneworld.zones"
    const val BROADCAST_CHANNEL = "zoneworld.broadcast"
    const val REPORT_CHANNEL = "zoneworld.report"
    const val ANNOUNCE_TOPIC = "world.announce"
    const val MAINTENANCE_TOPIC = "world.maintenance"
    const val PLAYER_ACTOR_TYPE = "zoneworld.player"
    const val ZONE_SPOT_TYPE = "zoneworld.zone"
    const val GATEWAY_STREAM = "zoneworld.gateway"
    const val OPS_STREAM = "zoneworld.ops"
    const val OPS_HANDLER_GROUP = "zoneworld.ops.handlers"
    const val BROADCAST_HANDLER_GROUP = "zoneworld.broadcast.handlers"
    const val NW_NE = "zone.border.zone-nw.zone-ne"
    const val NW_SW = "zone.border.zone-nw.zone-sw"
    const val NE_NW = "zone.border.zone-ne.zone-nw"
    const val NE_SE = "zone.border.zone-ne.zone-se"
    const val SW_NW = "zone.border.zone-sw.zone-nw"
    const val SW_SE = "zone.border.zone-sw.zone-se"
    const val SE_NE = "zone.border.zone-se.zone-ne"
    const val SE_SW = "zone.border.zone-se.zone-sw"

    fun borderTopic(from: String, to: String) = "zone.border.$from.$to"
}
