package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize
import kotlin.math.floor

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val ZONE = "Zone"
    const val DUNGEON = "Dungeon"
    const val BORDER = "border"
    const val CROSS = "cross"
    const val BORDER_SCENARIO = "border"
    const val RECONNECT = "reconnect"
    const val UPDATE = "update"
    val SCENARIOS = listOf(CROSS, BORDER_SCENARIO, RECONNECT, UPDATE)
}

object Rules {
    const val ZONE_WIDTH = 100
    const val ZONE_COUNT = 6
    const val BORDER_RADIUS = 10
    const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
    const val MAX_STEP = 10.0
    const val MAX_SPEED = MAX_STEP * 5
    const val FULL_BUDGET_AFTER_MS = 200
    const val ZONE_PREFIX = "zone-"

    const val WORLD_WIDTH = ZONE_WIDTH * ZONE_COUNT

    fun zoneId(x: Double): String = "$ZONE_PREFIX${floor(x / ZONE_WIDTH).toInt() + 1}"

    fun adjacentZones(id: String): List<String> {
        val number = id.removePrefix(ZONE_PREFIX).toInt()
        require(number in 1..ZONE_COUNT)
        return listOf(number - 1, number + 1)
            .filter { it in 1..ZONE_COUNT }
            .map { "$ZONE_PREFIX$it" }
    }

    fun borderTopic(source: String, target: String) = "$source:$target"
}

@JsonSerialize class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class JoinWorldRequest(val playerId: String = "", val x: Double = 0.0)

data class MoveRequest(val delta: Double = 0.0, val sequence: Long = 0)

data class Position(val x: Double = 0.0)

data class EnterZone(
    val position: Position = Position(),
    val sequence: Long = 0,
    val remainingMs: Int = 0,
)

data class WorldState(
    val zoneId: String = "",
    val nodeId: String = "",
    val x: Double = 0.0,
    val sequence: Long = 0,
)

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

object PlayerLeftReason {
    const val LEFT_ZONE = "left-zone"
    const val DISCONNECTED = "disconnected"
}

object BorderPresenceKind {
    const val APPEARED = "appeared"
    const val MOVED = "moved"
    const val LEFT = "left"
}

data class PlayerAppeared(val playerId: String = "", val position: Position = Position())

data class PlayerMoved(
    val playerId: String = "",
    val position: Position = Position(),
    val sequence: Long = 0,
)

data class PlayerLeft(val playerId: String = "", val reason: String = "")

data class BorderPresence(
    val sourceZoneId: String = "",
    val playerId: String = "",
    val position: Position = Position(),
    val sequence: Long = 0,
    val kind: String = "",
    val reason: String? = null,
)

data class OpenDungeonRequest(val dungeonId: String = "")

data class DungeonOpened(val dungeonId: String = "", val nodeId: String = "")
