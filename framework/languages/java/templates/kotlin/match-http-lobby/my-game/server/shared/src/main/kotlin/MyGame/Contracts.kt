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
    const val RANGE = 80.0
    const val WALL_X = 50.0
    const val WALL_MIN_Y = 0.0
    const val WALL_MAX_Y = 100.0
    const val OUT_OF_RANGE = "out-of-range"
    const val BLOCKED = "blocked-by-wall"
    const val NO_TARGET = "no-target"
    const val ADMISSION_REJECTED = "admission-rejected"
    const val INVALID_MOVE = "invalid-move"
    const val STALE_SEQUENCE = "stale-sequence"
    const val FINISHED = "match-finished"
}

@JsonSerialize class SessionInfoRequest

@JsonSerialize class RoomViewRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class MatchRequest(val playerId: String = "", val mode: String = "")

data class MatchResponse(val roomId: String = "", val sessionEndpoint: String = "")

data class JoinRoomRequest(val playerId: String = "", val roomId: String = "")

data class MoveRequest(val x: Double = 0.0, val y: Double = 0.0, val sequence: Long = 0)

data class ShootRequest(val targetId: String = "", val sequence: Long = 0)

data class PlayerView(
    val playerId: String = "",
    val x: Double = 0.0,
    val y: Double = 0.0,
    val hp: Int = 0,
)

data class RoomView(
    val roomId: String = "",
    val nodeId: String = "",
    val players: List<PlayerView> = emptyList(),
)

data class MatchEnded(val winner: String = "")

data class ShotRejected(val reason: String = "")

data class ActionRejected(val reason: String = "")
