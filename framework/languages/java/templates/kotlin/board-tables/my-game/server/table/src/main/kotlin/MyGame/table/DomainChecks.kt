package MyGame.table

import MyGame.*
import com.fasterxml.jackson.databind.JsonNode
import java.security.SecureRandom

internal fun verifyJsonContract() {
    val json = Json.mapper.valueToTree<JsonNode>(ActionRejected("sample", Int.MAX_VALUE))
    check(
        json.size() == 2 &&
            json.has("reason") &&
            json.has("retryAfterMs") &&
            !json.has("RetryAfterMs") &&
            !json.has("retry_after_ms") &&
            json.get("retryAfterMs").isInt &&
            json.get("retryAfterMs").intValue() == Int.MAX_VALUE
    ) {
        "ActionRejected JSON does not use a signed 32-bit retryAfterMs field"
    }
}

internal fun runDomainChecks(): Int {
    val checks =
        listOf(
            ::shuffledDeckContainsEveryCardOnce,
            ::injectedRandomProducesDeterministicOrder,
            ::settlementClearsAllPrivateHands,
            ::maximumBetAndOverLimitRejectionAreAtomic,
            ::potOverflowRejectionIsAtomic,
            ::payoutOverflowRejectionIsAtomic,
            ::handNumberOverflowRejectionIsAtomic,
        )
    checks.forEach { it() }
    return checks.size
}

private fun shuffledDeckContainsEveryCardOnce() {
    val table = table(secureRandomIndex())
    check(table.admit("p1") && table.admit("p2")) { "Could not prepare shuffled deck" }
    check(table.startDomain("p1") == null) { "Deck shuffle failed" }

    val cards = table.state.deck + table.state.seats.flatMap { it.cards }
    check(cards.size == Rules.DECK_SIZE && cards.toSet() == (0 until Rules.DECK_SIZE).toSet()) {
        "Shuffled deck did not contain every card exactly once"
    }
}

private fun injectedRandomProducesDeterministicOrder() {
    val table = table { bound -> bound - 1 }
    check(table.admit("p1") && table.admit("p2")) { "Could not prepare deterministic shuffle" }
    check(table.startDomain("p1") == null) { "Injected random source failed" }
    check(
        table.state.deck == (0 until 48).toList() &&
            table.state.seats[0].cards == listOf(51, 50) &&
            table.state.seats[1].cards == listOf(49, 48)
    ) {
        "Injected random source produced a different deterministic order"
    }
}

private fun settlementClearsAllPrivateHands() {
    val table = table { bound -> bound - 1 }
    repeat(Rules.SEATS) { check(table.admit("p$it")) { "Could not prepare settlement cards" } }
    check(table.startDomain("p0") == null) { "Could not start settlement hand" }

    var result: HandResult? = null
    repeat(Rules.SEATS) { index ->
        val outcome = table.betDomain("p$index", BetRequest(1, 0))
        check(outcome.rejection == null) { "Valid settlement bet was rejected" }
        if (index == Rules.SEATS - 1) result = table.settleDomain(outcome)
        else check(outcome.settlement == null) { "Settlement ended before every player acted" }
    }
    check(
        result != null &&
            result!!.seats.size == Rules.SEATS &&
            result!!.seats.all { it.cardCount == 0 } &&
            table.state.seats.all { it.cards.isEmpty() } &&
            (0 until Rules.SEATS).all { table.privateHand("p$it").isEmpty() }
    ) {
        "Settlement retained private cards"
    }
}

private fun maximumBetAndOverLimitRejectionAreAtomic() {
    val table = playingTable()
    table.state.seats[0].chips = Rules.MAX_CONTRACT_INTEGER
    val accepted = table.betDomain("p1", BetRequest(1, Rules.MAX_CONTRACT_INTEGER))
    check(
        accepted.rejection == null &&
            accepted.settlement == null &&
            table.state.pot == Rules.MAX_CONTRACT_INTEGER &&
            table.state.seats[0].chips == 0L
    ) {
        "Maximum in-range bet was not accepted"
    }

    val before = snapshot(table)
    val rejected = table.betDomain("p2", BetRequest(1, Rules.MAX_CONTRACT_INTEGER + 1L))
    check(rejected.rejection == Rules.INTEGER_OUT_OF_RANGE) {
        "Over-limit bet amount did not return integer-out-of-range"
    }
    unchanged(table, before, "Over-limit bet rejection changed state")
}

private fun potOverflowRejectionIsAtomic() {
    val table = playingTable()
    table.state.pot = Rules.MAX_CONTRACT_INTEGER
    val before = snapshot(table)
    val rejected = table.betDomain("p1", BetRequest(1, 1))
    check(rejected.rejection == Rules.INTEGER_OUT_OF_RANGE) {
        "Pot overflow did not return integer-out-of-range"
    }
    unchanged(table, before, "Pot overflow rejection changed state")
}

private fun payoutOverflowRejectionIsAtomic() {
    val table = playingTable()
    table.state.seats[0].chips = Rules.MAX_CONTRACT_INTEGER
    table.state.seats[0].cards = listOf(11, 24)
    table.state.seats[1].cards = listOf(0, 13)
    val first = table.betDomain("p1", BetRequest(1, 0))
    check(first.rejection == null && first.settlement == null) {
        "Could not prepare payout boundary"
    }

    val before = snapshot(table)
    val rejected = table.betDomain("p2", BetRequest(1, 1))
    check(rejected.rejection == Rules.INTEGER_OUT_OF_RANGE) {
        "Payout overflow did not return integer-out-of-range"
    }
    unchanged(table, before, "Payout overflow rejection changed state")
}

private fun handNumberOverflowRejectionIsAtomic() {
    val table = table(secureRandomIndex())
    check(table.admit("p1") && table.admit("p2")) { "Could not prepare hand-number boundary" }
    table.state.handNumber = Rules.MAX_CONTRACT_INTEGER
    val before = snapshot(table)
    check(table.startDomain("p1") == Rules.INTEGER_OUT_OF_RANGE) {
        "Maximum hand number did not reject the next hand"
    }
    unchanged(table, before, "Hand number overflow rejection changed state")
}

private fun playingTable(): Table {
    val table = table(secureRandomIndex())
    check(table.admit("p1") && table.admit("p2")) { "Could not prepare bet boundary" }
    table.state.seats[0].cards = listOf(11, 24)
    table.state.seats[1].cards = listOf(0, 13)
    table.state.handNumber = 1L
    table.state.inHand = true
    table.state.turnSeat = 0
    return table
}

private fun table(randomIndex: (Int) -> Int): Table = Table.forDomainChecks(randomIndex)

private fun secureRandomIndex(): (Int) -> Int {
    val random = SecureRandom()
    return random::nextInt
}

private fun snapshot(table: Table): JsonNode = Json.mapper.valueToTree<JsonNode>(table.state)

private fun unchanged(table: Table, before: JsonNode, message: String) {
    check(snapshot(table) == before) { message }
}
