package MyGame

import com.fasterxml.jackson.databind.annotation.JsonSerialize

object Names {
    const val LANGUAGE = "kotlin"
    const val MESH = "my-game-tables"
    const val STREAM = "players"
    const val PLAYER = "Player"
    const val TABLE = "Table"
    val SCENARIOS = listOf("table", "reconnect", "update")
}

object Rules {
    const val SEATS = 4
    const val INITIAL_CHIPS = 1000
    const val DECK_SIZE = 52
    const val RANKS = 13
    const val MIN_RANK = 2
    const val CARDS = 2
    const val MIN_PLAYERS = 2
    const val NO_SEAT = -1
    const val TABLE_FULL = "table-full"
    const val NO_HAND = "no-hand"
    const val HAND_IN_PROGRESS = "hand-in-progress"
    const val NOT_ENOUGH_PLAYERS = "not-enough-players"
    const val NOT_YOUR_TURN = "not-your-turn"
    const val INVALID_AMOUNT = "invalid-amount"
    const val NOT_SEATED = "not-seated"
}

@JsonSerialize class SessionInfoRequest

data class JoinTableRequest(val playerId: String = "", val tableId: String = "")

@JsonSerialize class TableViewRequest

@JsonSerialize class StartHandRequest

data class BetRequest(val handNumber: Int = 0, val amount: Int = 0)

data class SessionInfo(val sessionId: String = "")

data class SeatView(
    val seat: Int = 0,
    val playerId: String = "",
    val chips: Int = 0,
    val acted: Boolean = false,
    val cardCount: Int = 0,
)

data class TableView(
    val tableId: String = "",
    val nodeId: String = "",
    val actorId: String = "",
    val objectGeneration: Long = 0,
    val yourSeat: Int = Rules.NO_SEAT,
    val handNumber: Int = 0,
    val inHand: Boolean = false,
    val turnSeat: Int = Rules.NO_SEAT,
    val pot: Int = 0,
    val seats: List<SeatView> = emptyList(),
)

data class HandDealt(val handNumber: Int = 0, val cards: List<Int> = emptyList())

data class RevealedHand(
    val seat: Int = 0,
    val playerId: String = "",
    val cards: List<Int> = emptyList(),
)

data class HandResult(
    val handNumber: Int = 0,
    val pot: Int = 0,
    val winners: List<Int> = emptyList(),
    val hands: List<RevealedHand> = emptyList(),
    val seats: List<SeatView> = emptyList(),
)

data class ActionRejected(val reason: String = "")
