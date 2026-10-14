package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val ZONE = "Zone"
    const val DUNGEON = "Dungeon"
    const val BORDER = "border"
    const val BORDER_TIMER = "border-tick"
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
    const val TICK_MILLIS = 100L
    const val ZONE_PREFIX = "zone-"

    fun zoneId(x: Int): String {
        require(x in 0 until ZONE_WIDTH * ZONE_COUNT) { "Position outside world" }
        return "$ZONE_PREFIX${x / ZONE_WIDTH + 1}"
    }

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

data class JoinWorldRequest(val playerId: String = "", val x: Int = 0)

data class MoveRequest(val x: Int = 0, val sequence: Int = 0)

data class EnterZone(val x: Int = 0, val sequence: Int = 0)

data class WorldState(
    val zoneId: String = "",
    val nodeId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class BorderPresence(
    val sourceZoneId: String = "",
    val actorId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class BorderVisible(
    val observerZoneId: String = "",
    val sourceZoneId: String = "",
    val actorId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class OpenDungeonRequest(val dungeonId: String = "")

data class DungeonOpened(val dungeonId: String = "", val nodeId: String = "")
