package MyGame

object Names {
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val TOWN = "Town"
    const val DUNGEON = "Dungeon"
    const val LANGUAGE = "kotlin"
    val SCENARIOS = listOf("channel", "dungeon", "reconnect", "update")
    const val CHANNEL_COUNT = 2
    const val MAXIMUM_PARTY_MEMBERS = 4
    const val MAXIMUM_INTEGER = 9_007_199_254_740_991L
    const val FINAL_STAGE = 3L

    fun townId(channel: Long): String {
        require(channel in 1L..CHANNEL_COUNT.toLong()) { "Invalid town channel" }
        return "town-$channel"
    }

    fun dungeonId(runId: String) = "dungeon-$runId"

    fun validId(value: String, prefix: String): Boolean =
        value.startsWith(prefix) &&
            value.length == prefix.length + 32 &&
            value.substring(prefix.length).all { it in '0'..'9' || it in 'a'..'f' }
}

object Rejection {
    const val JOIN_REJECTED = "join-rejected"
    const val INVALID_PARTY = "invalid-party"
    const val NOT_LEADER = "not-leader"
    const val RUN_MISMATCH = "run-mismatch"
    const val RUN_IN_PROGRESS = "run-in-progress"
    const val RUN_NOT_ACTIVE = "run-not-active"
    const val INTEGER_OUT_OF_RANGE = "integer-out-of-range"
    const val INVALID_CHANNEL = "invalid-channel"
    const val PACKET_NOT_ALLOWED = "packet-not-allowed"
}

enum class RunPhase { Forming, Preparing, Entering, Active, Completed }

data class JoinTownRequest(val playerId: String = "", val channel: Long = 1)

data class ChangeChannelRequest(val channel: Long = 1)

class CreatePartyRequest

data class JoinPartyRequest(val partyId: String = "")

class EnterDungeonRequest

class AdvanceRequest

class ViewRequest

class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class PartyView(
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val phase: RunPhase = RunPhase.Forming,
    val runId: String? = null,
)

data class TownView(
    val townId: String = "",
    val nodeId: String = "",
    val players: List<String> = emptyList(),
    val parties: List<PartyView> = emptyList(),
)

data class DungeonView(
    val dungeonId: String = "",
    val nodeId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val stage: Long = 0,
    val runId: String = "",
    val phase: RunPhase = RunPhase.Forming,
)

data class PrepareDungeon(
    val runId: String = "",
    val partyId: String = "",
    val ownerSpotId: String = "",
    val members: List<String> = emptyList(),
)

data class DungeonPrepared(
    val dungeonId: String = "",
    val runId: String = "",
    val reason: String? = null,
)

data class JoinDungeon(val dungeonId: String = "", val runId: String = "")

data class ActionRejected(val reason: String = "", val retryAfterMs: Int = 0)

data class RunStarted(val runId: String = "")

data class RunCompleted(val runId: String = "")

data class PartyViewRequest(val playerId: String = "")
