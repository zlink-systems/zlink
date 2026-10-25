package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-arena"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val ROOM = "Room"
    const val MATCH_PATH = "/match"
    const val ROOM_PREFIX = "room-"
    const val DEFAULT_MODE = "duel"
    val SCENARIOS = Scenario.entries.map { it.id }
}

enum class Scenario(val id: String) {
    LOBBY("lobby"),
    HIT("hit"),
    WALL("wall"),
    INPUT("input"),
    RECONNECT("reconnect"),
    UPDATE("update");

    companion object {
        fun parse(id: String) = entries.single { it.id == id }
    }
}

object Roles {
    const val MATCH = "match"
    const val SESSION = "session"
    const val LOBBY = "http-lobby"
    const val EXTERNAL = "external"
    const val LOCATION = "location-store"
}

object HttpProtocol {
    const val POST = "POST"
    const val CONTENT_TYPE = "Content-Type"
    const val JSON = "application/json"
    const val OK = 200
    const val BAD_REQUEST = 400
    const val NOT_FOUND = 404
    const val UNAVAILABLE = 503
}

object Rules {
    const val PLAYERS = 2
    const val INITIAL_HP = 100
    const val DAMAGE = 25
    const val FIRE_INTERVAL_MS = 250L
    const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
    const val MAP_MIN = 0.0
    const val MAP_MAX = 100.0
    const val MAX_STEP = 2.0
    const val MOVE_SPEED = 10.0
    const val RANGE = 80.0
    const val SPAWN_AX = 40.0
    const val SPAWN_AY = 10.0
    const val SPAWN_BX = 60.0
    const val SPAWN_BY = 10.0
    const val WALL_X = 50.0
    const val WALL_MIN_Y = 20.0
    const val WALL_MAX_Y = 60.0
    const val OUT_OF_RANGE = "out-of-range"
    const val BLOCKED = "blocked-by-wall"
    const val NO_TARGET = "no-target"
    const val ADMISSION_REJECTED = "admission-rejected"
    const val INVALID_MOVE = "invalid-move"
    const val STEP_TOO_LARGE = "step-too-large"
    const val OUTSIDE_MAP = "outside-map"
    const val MOVE_BLOCKED_BY_WALL = "move-blocked-by-wall"
    const val MOVE_BUDGET = "move-budget"
    const val FIRE_TOO_SOON = "fire-too-soon"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    val WALLS = listOf(Wall(WALL_X, WALL_MIN_Y, WALL_MAX_Y))
    const val STALE_SEQUENCE = "stale-sequence"
    const val FINISHED = "match-finished"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
    const val PLAYING = "Playing"
    const val FINISHED_PHASE = "Finished"
}

@JsonSerialize class SessionInfoRequest

@JsonSerialize class RoomViewRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class MatchRequest(val playerId: String = "", val mode: String? = null)

data class MatchResponse(val roomId: String = "", val sessionEndpoint: String = "")

data class JoinRoomRequest(val playerId: String = "", val roomId: String = "")

data class MoveRequest(val dx: Double = 0.0, val dy: Double = 0.0, val sequence: Long = 0)

data class ShootRequest(val targetId: String = "", val sequence: Long = 0)

data class MoveResult(val x: Double = 0.0, val y: Double = 0.0, val sequence: Long = 0)

data class ShotResult(val outcome: String = "", val targetId: String = "", val sequence: Long = 0)

data class PrepareRoom(val matchId: String = "", val members: List<String> = emptyList())

data class RoomPrepared(val matchId: String = "")

data class PlayerView(
    val playerId: String = "",
    val x: Double = 0.0,
    val y: Double = 0.0,
    val hp: Int = Rules.INITIAL_HP,
    val lastSequence: Long = 0,
)

data class RoomView(
    val roomId: String = "",
    val nodeId: String = "",
    val players: List<PlayerView> = emptyList(),
    val phase: String = Rules.PLAYING,
    val winner: String? = null,
)

data class MatchEnded(val winner: String = "")

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class Wall(val x: Double, val minY: Double, val maxY: Double)
