package MyGame

object Names {
    const val MESH = "my-game-arena"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val ROOM = "Room"
    const val MATCHMAKING = "Matchmaking"
    const val MATCHMAKING_ID = "matchmaking"
    const val DEFAULT_MODE = "duel"
    const val ROOM_PREFIX = "room-"
    const val LANGUAGE = "kotlin"
    val SCENARIOS = listOf("hit", "reconnect", "update")
}

object Game {
    const val MATCHMAKING_CAPACITY = 1
    const val PLAYERS_PER_ROOM = 2
    const val INITIAL_HP = 100
    const val SHOT_DAMAGE = 25
    const val SHOT_RANGE = 80.0
    val WALLS = listOf(Wall(50.0, 0.0, 100.0))
}

data class Wall(val x: Double, val minY: Double, val maxY: Double)

object Rejection {
    const val ADMISSION_REJECTED = "admission-rejected"
    const val OUT_OF_RANGE = "out-of-range"
    const val BLOCKED_BY_WALL = "blocked-by-wall"
    const val NO_TARGET = "no-target"
    const val INVALID_MOVE = "invalid-move"
    const val STALE_SEQUENCE = "stale-sequence"
    const val MATCH_FINISHED = "match-finished"
}

class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class QueueRequest(val playerId: String = "", val mode: String = Names.DEFAULT_MODE)

data class RoomAdmission(val playerId: String = "", val roomId: String = "")

data class RoomAssignment(val roomId: String = "")

data class MatchFound(val roomId: String = "")

data class MoveRequest(val x: Double = 0.0, val y: Double = 0.0, val sequence: Long = 0)

data class ShootRequest(val targetId: String = "", val sequence: Long = 0)

class RoomViewRequest

data class PlayerView(
    val playerId: String = "",
    val x: Double = 0.0,
    val y: Double = 0.0,
    val hp: Int = Game.INITIAL_HP,
)

data class RoomView(
    val roomId: String = "",
    val nodeId: String = "",
    val players: List<PlayerView> = emptyList(),
)

data class ShotRejected(val reason: String = "")

data class ActionRejected(val reason: String = "")

data class MatchEnded(val winner: String = "")
