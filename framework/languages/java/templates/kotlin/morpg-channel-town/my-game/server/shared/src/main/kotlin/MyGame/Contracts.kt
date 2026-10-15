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

    fun townId(channel: Int): String {
        require(channel in 1..CHANNEL_COUNT) { "Invalid town channel" }
        return "town-$channel"
    }

    fun dungeonId(partyId: String) = "dungeon-$partyId"
}

object Rejection {
    const val JOIN_REJECTED = "JOIN_REJECTED"
    const val INVALID_PARTY = "INVALID_PARTY"
    const val NOT_LEADER = "NOT_LEADER"
}

data class JoinTownRequest(val playerId: String = "", val channel: Int = 1)

data class ChangeChannelRequest(val channel: Int = 1)

data class CreatePartyRequest(val partyId: String = "")

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

data class PartyView(val partyId: String = "", val members: List<String> = emptyList())

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
    val stage: Int = 0,
)

data class PrepareDungeon(val partyId: String = "", val members: List<String> = emptyList())

data class DungeonPrepared(val dungeonId: String = "")

data class JoinDungeon(val dungeonId: String = "", val partyId: String = "")

data class ActionRejected(val reason: String = "")
