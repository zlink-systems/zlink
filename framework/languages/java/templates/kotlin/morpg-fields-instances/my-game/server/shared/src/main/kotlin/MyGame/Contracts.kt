package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val FIELD = "Field"
    const val PARTY = "Party"
    const val DUNGEON = "Dungeon"
    const val BORDER = "border"
    const val BORDER_TIMER = "border-tick"
    const val FIELD_SCENARIO = "field"
    const val DUNGEON_SCENARIO = "dungeon"
    const val RECONNECT = "reconnect"
    const val UPDATE = "update"
    val SCENARIOS = listOf(FIELD_SCENARIO, DUNGEON_SCENARIO, RECONNECT, UPDATE)
}

object Rules {
    const val FIELD_WIDTH = 100
    const val FIELD_COUNT = 4
    const val BORDER_RADIUS = 10
    const val TICK_MILLIS = 100L
    const val MAXIMUM_PARTY_MEMBERS = 4
    const val FINAL_STAGE = 3
    const val MAX_SAFE_INTEGER = 9_007_199_254_740_991L
    const val MAX_STEP = 10.0
    const val MAX_SPEED = MAX_STEP * 5
    const val FULL_BUDGET_MS = 200

    fun dungeonId(runId: String) = "dungeon-$runId"

    const val FIELD_PREFIX = "field-"

    fun fieldId(x: Double): String {
        require(x.isFinite() && x >= 0 && x < FIELD_WIDTH * FIELD_COUNT) {
            "Position outside world"
        }
        return "$FIELD_PREFIX${(x / FIELD_WIDTH).toInt() + 1}"
    }

    fun adjacentFields(id: String): List<String> {
        val number = id.removePrefix(FIELD_PREFIX).toInt()
        require(number in 1..FIELD_COUNT)
        return listOf(number - 1, number + 1)
            .filter { it in 1..FIELD_COUNT }
            .map { "$FIELD_PREFIX$it" }
    }

    fun borderTopic(source: String, target: String) = "$source:$target"
}

enum class RunPhase {
    Forming,
    Preparing,
    Entering,
    Active,
    Completed,
}

@JsonSerialize class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class JoinWorldRequest(val playerId: String = "", val x: Double = 0.0)

data class MoveRequest(val delta: Double = 0.0, val sequence: Long = 0L)

data class EnterField(val x: Double = 0.0, val sequence: Long = 0L, val remainingMs: Int = 0)

data class WorldState(
    val fieldId: String = "",
    val nodeId: String = "",
    val x: Double = 0.0,
    val sequence: Long = 0L,
)

data class BorderPresence(
    val sourceFieldId: String = "",
    val playerId: String = "",
    val x: Double = 0.0,
    val sequence: Long = 0L,
)

data class BorderVisible(
    val observerFieldId: String = "",
    val sourceFieldId: String = "",
    val playerId: String = "",
    val x: Double = 0.0,
    val sequence: Long = 0L,
)

@JsonSerialize class CreatePartyRequest

data class JoinPartyRequest(val partyId: String = "")

@JsonSerialize class EnterDungeonRequest

@JsonSerialize class ViewRequest

@JsonSerialize class AdvanceRequest

data class PartyView(
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val leaderId: String = "",
    val phase: RunPhase = RunPhase.Forming,
    val runId: String? = null,
)

data class PrepareDungeon(
    val runId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
)

data class DungeonPrepared(
    val dungeonId: String = "",
    val runId: String = "",
    val rejection: ActionRejected? = null,
)

data class JoinDungeon(val dungeonId: String = "", val runId: String = "")

data class DungeonView(
    val dungeonId: String = "",
    val nodeId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val stage: Int = 0,
    val runId: String = "",
    val phase: RunPhase = RunPhase.Preparing,
)

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

object Rejection {
    const val INVALID_PARTY = "invalid-party"
    const val NOT_LEADER = "not-leader"
    const val RUN_MISMATCH = "run-mismatch"
    const val RUN_IN_PROGRESS = "run-in-progress"
    const val RUN_NOT_ACTIVE = "run-not-active"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
    const val NON_FINITE = "non-finite"
    const val OUTSIDE_WORLD = "outside-world"
    const val STEP_TOO_LARGE = "step-too-large"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val STALE_SEQUENCE = "stale-sequence"
    const val MOVE_BUDGET = "move-budget"
}

data class PartyCommand(val actorId: String = "", val join: Boolean = false, val enter: Boolean = false)

data class PartyCommandResult(val view: PartyView? = null, val rejection: ActionRejected? = null)

data class DungeonMemberEntered(val runId: String = "", val actorId: String = "")

data class RunStarted(val runId: String = "")

data class RunCompleted(val runId: String = "")
