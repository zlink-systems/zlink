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
    val SCENARIOS = listOf("hit", "wall", "input", "reconnect", "update")
}

object Game {
    const val MATCHMAKING_CAPACITY = 1
    const val PLAYERS_PER_ROOM = 2
    const val INITIAL_HP = 100
    const val SHOT_DAMAGE = 25
    const val SHOT_RANGE = 80.0
    const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
    const val MAP_MIN_X = 0.0
    const val MAP_MAX_X = 100.0
    const val MAP_MIN_Y = 0.0
    const val MAP_MAX_Y = 100.0
    const val MAX_STEP = 2.0
    const val MAX_SPEED = 10.0
    const val FIRE_INTERVAL_MS = 250L
    const val FIRST_SPAWN_X = 40.0
    const val FIRST_SPAWN_Y = 10.0
    const val SECOND_SPAWN_X = 60.0
    const val SECOND_SPAWN_Y = 10.0
    const val WALL_X = 50.0
    const val WALL_MIN_Y = 20.0
    const val WALL_MAX_Y = 60.0
    const val PLAYING_PHASE = "Playing"
    const val FINISHED_PHASE = "Finished"
    val WALLS = listOf(Wall(WALL_X, WALL_MIN_Y, WALL_MAX_Y))
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
    const val STEP_TOO_LARGE = "step-too-large"
    const val OUTSIDE_MAP = "outside-map"
    const val MOVE_BLOCKED_BY_WALL = "move-blocked-by-wall"
    const val MOVE_BUDGET = "move-budget"
    const val FIRE_TOO_SOON = "fire-too-soon"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
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

data class MoveRequest(val dx: Double = 0.0, val dy: Double = 0.0, val sequence: Long = 0)

data class ShootRequest(val targetId: String = "", val sequence: Long = 0)

class RoomViewRequest

data class PlayerView(
    val playerId: String = "",
    val x: Double = 0.0,
    val y: Double = 0.0,
    val hp: Int = Game.INITIAL_HP,
    val lastSequence: Long = 0,
)

data class RoomView(
    val roomId: String = "",
    val nodeId: String = "",
    val phase: String = Game.PLAYING_PHASE,
    val winner: String? = null,
    val players: List<PlayerView> = emptyList(),
)

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class MatchEnded(val winner: String = "")
