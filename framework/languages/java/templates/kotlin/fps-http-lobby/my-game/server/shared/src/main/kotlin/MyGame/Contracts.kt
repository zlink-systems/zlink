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
}

object Game {
    const val MATCHMAKING_CAPACITY = 1
    const val MAX_SEQUENCE = 9_007_199_254_740_991L
    const val PLAYERS_PER_ROOM = 2
    const val INITIAL_HP = 100
    const val SHOT_DAMAGE = 25
    const val SHOT_RANGE = 80.0
    const val MAX_STEP = 2.0
    const val MAX_SPEED = 10.0
    const val FIRE_INTERVAL_MS = 250
    const val EYE_HEIGHT = 1.6
    const val PLAYER_HALF_WIDTH = 0.5
    const val PLAYER_HEIGHT = 2.0
}

object Rejection {
    const val BLOCKED_BY_WALL = "blocked-by-wall"
    const val ADMISSION_REJECTED = "admission-rejected"
    const val VERTICAL_MOVE = "vertical-move"
    const val NON_FINITE = "non-finite"
    const val STEP_TOO_LARGE = "step-too-large"
    const val OUTSIDE_MAP = "outside-map"
    const val MOVE_BLOCKED_BY_WALL = "move-blocked-by-wall"
    const val MOVE_BUDGET = "move-budget"
    const val INVALID_AIM = "invalid-aim"
    const val FIRE_TOO_SOON = "fire-too-soon"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val STALE_SEQUENCE = "stale-sequence"
    const val MATCH_FINISHED = "match-finished"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
}

object ShotOutcome {
    const val HIT = "hit"
    const val MISS = "miss"
    const val BLOCKED_BY_WALL = Rejection.BLOCKED_BY_WALL
}

enum class MatchPhase {
    Waiting,
    Playing,
    Finished
}

class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class QueueRequest(
    val playerId: String = "",
    val mode: String = Names.DEFAULT_MODE,
)

data class RoomAdmission(
    val playerId: String = "",
    val roomId: String = "",
)

data class RoomAssignment(val roomId: String = "")

data class MatchFound(val roomId: String = "")

data class MoveRequest(
    val delta: Vec3 = Vec3(),
    val sequence: Long = 0,
)

data class MoveResult(
    val position: Vec3 = Vec3(),
    val sequence: Long = 0,
)

data class ShootRequest(
    val aimDirection: Vec3 = Vec3(),
    val sequence: Long = 0,
)

class RoomViewRequest

data class ShotResult(
    val outcome: String = "",
    val targetId: String? = null,
    val sequence: Long = 0,
)

data class ShotRejected(
    val reason: String = "",
    val retryAfterMs: Int = 0,
)

data class ActionRejected(
    val reason: String = "",
    val retryAfterMs: Int = 0,
)

data class PlayerView(
    val playerId: String = "",
    val position: Vec3 = Vec3(),
    val hp: Int = Game.INITIAL_HP,
    val lastSequence: Long = 0,
)

data class RoomView(
    val roomId: String = "",
    val nodeId: String = "",
    val players: List<PlayerView> = emptyList(),
    val phase: MatchPhase = MatchPhase.Waiting,
    val winner: String? = null,
)

data class MatchEnded(val winner: String = "")
