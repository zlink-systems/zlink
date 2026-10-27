package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val PARTY = "Party"
    const val DUNGEON = "Dungeon"
    const val PARTY_PREFIX = "party-"
    const val DUNGEON_PREFIX = "dungeon-"
    fun newPartyId() = PARTY_PREFIX + java.util.UUID.randomUUID().toString().replace("-", "")
    fun newRunId() = "run-" + java.util.UUID.randomUUID().toString().replace("-", "")
    fun dungeonId(runId: String) = DUNGEON_PREFIX + runId
    val SCENARIOS = Scenario.entries.map { it.id }
}

enum class Scenario(val id: String) {
    PARTY("party"),
    RECONNECT("reconnect"),
    UPDATE("update"),
}

enum class NodeRole(val id: String) {
    EXTERNAL("external"),
    SESSION("session"),
    LOBBY("lobby"),
    INSTANCE("instance"),
    LOCATION_STORE("location-store");

    val application
        get() = this == SESSION || this == LOBBY || this == INSTANCE

    companion object {
        fun parse(id: String) = entries.single { it.id == id }
    }
}

object Rules {
    const val MAX_MEMBERS = 4
    const val MAXIMUM_INTEGER = 9_007_199_254_740_991L
    const val FINAL_STAGE = 3
    const val RUN_MISMATCH = "run-mismatch"
    const val RUN_NOT_ACTIVE = "run-not-active"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val NOT_READY = "not-ready"
    const val NOT_LEADER = "not-leader"
    const val INVALID_PARTY = "invalid-party"
    const val JOIN_REJECTED = "join-rejected"
}

@JsonSerialize class SessionInfoRequest

@JsonSerialize class LobbyView

@JsonSerialize class ReadyRequest

@JsonSerialize class EnterDungeonRequest

@JsonSerialize class AdvanceRequest

@JsonSerialize class ViewRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class JoinLobbyRequest(val playerId: String = "")

@JsonSerialize class CreatePartyRequest

data class JoinPartyRequest(val partyId: String = "")

data class PartyJoin(val partyId: String = "", val create: Boolean = false)

data class PartyView(
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val ready: List<Boolean> = emptyList(),
    val phase: RunPhase = RunPhase.Forming,
    val runId: String? = null,
)

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class JoinDungeon(val dungeonId: String = "", val runId: String = "")

data class PrepareDungeon(val runId: String = "", val partyId: String = "", val members: List<String> = emptyList())

data class DungeonPrepared(val dungeonId: String = "", val runId: String = "")

data class DungeonView(
    val dungeonId: String = "",
    val nodeId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val stage: Long = 0,
    val runId: String? = null,
    val phase: RunPhase = RunPhase.Preparing,
)

enum class RunPhase { Forming, Preparing, Entering, Active, Completed }
data class RunStarted(val runId: String = "")
data class RunCompleted(val runId: String = "")
data class EnterPartyRun(val playerId: String = "")

data class DungeonMemberEntered(val runId: String = "", val playerId: String = "")
