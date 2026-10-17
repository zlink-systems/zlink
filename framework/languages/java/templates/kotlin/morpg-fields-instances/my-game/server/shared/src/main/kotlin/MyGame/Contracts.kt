package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-world"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val FIELD = "Field"
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

    fun dungeonId(partyId: String) = "dungeon-$partyId"

    const val FIELD_PREFIX = "field-"

    fun fieldId(x: Int): String {
        require(x in 0 until FIELD_WIDTH * FIELD_COUNT) { "Position outside world" }
        return "$FIELD_PREFIX${x / FIELD_WIDTH + 1}"
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

@JsonSerialize class SessionInfoRequest

data class SessionInfo(
    val sessionId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
)

data class JoinWorldRequest(val playerId: String = "", val x: Int = 0)

data class MoveRequest(val x: Int = 0, val sequence: Int = 0)

data class EnterField(val x: Int = 0, val sequence: Int = 0)

data class WorldState(
    val fieldId: String = "",
    val nodeId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class BorderPresence(
    val sourceFieldId: String = "",
    val playerId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class BorderVisible(
    val observerFieldId: String = "",
    val sourceFieldId: String = "",
    val playerId: String = "",
    val x: Int = 0,
    val sequence: Int = 0,
)

data class CreatePartyRequest(val partyId: String = "")

data class JoinPartyRequest(val partyId: String = "")

@JsonSerialize class EnterDungeonRequest

@JsonSerialize class ViewRequest

@JsonSerialize class AdvanceRequest

data class PartyView(val partyId: String = "", val members: List<String> = emptyList())

data class FieldView(
    val fieldId: String = "",
    val nodeId: String = "",
    val players: List<String> = emptyList(),
    val parties: List<PartyView> = emptyList(),
)

data class PrepareDungeon(val partyId: String = "", val members: List<String> = emptyList())

data class DungeonPrepared(val dungeonId: String = "")

data class JoinDungeon(val dungeonId: String = "", val partyId: String = "")

data class DungeonView(
    val dungeonId: String = "",
    val nodeId: String = "",
    val partyId: String = "",
    val members: List<String> = emptyList(),
    val stage: Int = 0,
)

data class ActionRejected(val reason: String = "")

object Rejection {
    const val INVALID_PARTY = "INVALID_PARTY"
    const val NOT_LEADER = "NOT_LEADER"
}
