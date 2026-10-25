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
    const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
    const val MAX_REPLICAS_PER_REGION = 2
    const val MAX_STEP = 10
    const val REGION_MIN_X = 0
    const val REGION_MAX_X = 100
    const val MAX_SPEED = MAX_STEP * 5.0
    val ALLOWED_REGIONS = listOf("forest", "desert")
    const val MOVEMENT = "movement"
    const val FIRST_REGION_INDEX = 1
    const val REGION_SCENARIO = "region"
    const val DUNGEON_SCENARIO = "dungeon"
    const val RECONNECT = "reconnect"
    const val UPDATE = "update"
    val SCENARIOS = listOf(REGION_SCENARIO, MOVEMENT, DUNGEON_SCENARIO, RECONNECT, UPDATE)

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

data class PlayerPosition(val playerId: String = "", val x: Double = 0.0, val sequence: Long = 0)

data class RegionView(
    val regionId: String = "",
    val nodeId: String = "",
    val players: List<PlayerPosition> = emptyList(),
)

data class MoveRequest(val delta: Double = 0.0, val sequence: Long = 0)

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class RegionUnavailable(val region: String = "", val reason: String = "")

data class OpenDungeonRequest(val dungeonId: String = "")

data class DungeonOpened(val dungeonId: String = "", val nodeId: String = "")

object Roles {
    const val EXTERNAL = "external"
    const val LOCATION = "location-store"
    const val SESSION = "session"
    const val REGION = "region"
    const val INSTANCE = "instance"
}
