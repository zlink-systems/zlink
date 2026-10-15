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
    const val NOT_READY = "not-ready"
    const val NOT_LEADER = "not-leader"
    const val INVALID_PARTY = "invalid-party"
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

data class CreatePartyRequest(val partyId: String = "")

data class JoinPartyRequest(val partyId: String = "")

data class PartyJoin(val partyId: String = "", val create: Boolean = false)

data class PartyView(
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val ready: List<Boolean> = emptyList(),
)

data class ActionRejected(val reason: String = "")

data class JoinDungeon(val dungeonId: String = "", val partyId: String = "")

data class PrepareDungeon(val partyId: String = "", val members: List<String> = emptyList())

data class DungeonPrepared(val dungeonId: String = "")

data class DungeonView(
    val dungeonId: String = "",
    val nodeId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val stage: Int = 0,
)
