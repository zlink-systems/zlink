package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val REGION = "Region"
    const val DUNGEON = "Dungeon"
    const val REGION_FULL = "region-full"
    const val FIRST_REGION_INDEX = 1
    const val REGION_SCENARIO = "region"
    const val DUNGEON_SCENARIO = "dungeon"
    const val RECONNECT = "reconnect"
    const val UPDATE = "update"
    val SCENARIOS = listOf(REGION_SCENARIO, DUNGEON_SCENARIO, RECONNECT, UPDATE)

    fun regionId(region: String, index: Int): String {
        require(region.isNotBlank() && index >= FIRST_REGION_INDEX) { "Invalid region identity" }
        return "$region-$index"
    }
}

@JsonSerialize class SessionInfoRequest

@JsonSerialize class ViewRequest

@JsonSerialize class DungeonInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class EnterRegionRequest(val playerId: String = "", val region: String = "")

data class JoinRegion(val region: String = "", val index: Int = 0)

data class RegionFull(val reason: String = "", val region: String = "", val index: Int = 0)

data class PlayerPosition(val playerId: String = "", val x: Int = 0)

data class RegionView(
    val regionId: String = "",
    val nodeId: String = "",
    val players: List<PlayerPosition> = emptyList(),
)

data class MoveRequest(val x: Int = 0, val sequence: Long = 0)

data class OpenDungeonRequest(val dungeonId: String = "")

data class DungeonOpened(val dungeonId: String = "", val nodeId: String = "")

object Roles {
    const val EXTERNAL = "external"
    const val LOCATION = "location-store"
    const val SESSION = "session"
    const val REGION = "region"
    const val INSTANCE = "instance"
}
