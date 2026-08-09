package systems.zlink.samples.kotlin.zoneworld.shared

object Messages {
    data class PlayerView(val playerId: String, val x: Int, val y: Int, val zoneId: String, val isBot: Boolean)
    data class NodeView(
        val nodeId: String,
        val registered: Boolean,
        val connected: Boolean,
        val maintenance: Boolean,
        val zones: List<String>,
        val playerCount: Int,
    )

    data class JoinWorldReq(val playerId: String)
    data class JoinWorldRes(val playerId: String, val zoneId: String, val x: Int, val y: Int, val error: String? = null)
    data class MoveMsg(val x: Int, val y: Int)
    data class ZoneStateNotify(val zoneId: String, val tick: Long, val players: List<PlayerView>)
    data class ZoneChangedNotify(val playerId: String, val zoneId: String)
    data class WorldAnnounceNotify(val announcementId: String, val text: String)
    data class MoveRejectedNotify(val reason: String, val x: Int, val y: Int)

    class WatchNodesReq
    data class WatchNodesRes(val nodes: List<NodeView>)
    data class AnnounceWorldReq(val text: String)
    data class AnnounceWorldRes(val announcementId: String)
    data class SetMaintenanceReq(val nodeId: String, val enabled: Boolean)
    data class SetMaintenanceRes(val nodeId: String, val enabled: Boolean, val zones: List<String>, val error: String? = null)
    data class NodeDiagnosticsReq(val nodeId: String)
    data class NodeDiagnosticsRes(
        val nodeId: String,
        val zones: List<String>,
        val playerCount: Int,
        val maintenance: Boolean,
        val error: String? = null,
    )

    data class WorldAnnounceEvent(val announcementId: String, val text: String)
    data class NodeMaintenanceChangedEvent(val nodeId: String, val enabled: Boolean)
    data class DeliverAnnounceMsg(val announcementId: String, val text: String)
    class BotTickMsg
    data class EnterWorldReq(val x: Int, val y: Int, val isBot: Boolean, val dirX: Int, val dirY: Int)
    data class EnterWorldRes(val zoneId: String, val x: Int, val y: Int, val error: String? = null)
    data class ReportSpotEventMsg(val nodeId: String, val kind: String, val detail: String, val occurredAt: String)
    data class ReportNodeStatusMsg(val nodeId: String, val zones: List<String>, val playerCount: Int, val maintenance: Boolean)
    data class ZoneBorderEvent(val fromZoneId: String, val toZoneId: String, val tick: Long, val players: List<PlayerView>)
    data class EnterZoneMsg(
        val playerId: String,
        val x: Int,
        val y: Int,
        val isBot: Boolean,
        val initialEntry: Boolean,
        val fromNodeId: String,
    )
    data class EnterZoneRes(val zoneId: String, val error: String? = null)
    data class UpdatePositionMsg(val playerId: String, val x: Int, val y: Int, val isBot: Boolean)
    data class DeliverZoneStateMsg(val zoneId: String, val tick: Long, val players: List<PlayerView>)
}
