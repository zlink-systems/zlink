package MyGame.e2e

import MyGame.*
import java.util.UUID

private fun assertChips(view: TableView) =
    check(view.seats.sumOf { it.chips } + view.pot == Rules.INITIAL_CHIPS * 2)

private fun same(before: TableView, after: TableView) {
    check(after.copy(nodeId = before.nodeId) == before) { "State changed: $before -> $after" }
}

private fun result(result: HandResult, cards: List<List<Int>>, bets: List<Int>) {
    check(result.hands.map { it.cards } == cards)
    val scores = cards.map { it.sumOf { card -> card % Rules.RANKS + Rules.MIN_RANK } }
    val winners = scores.indices.filter { scores[it] == scores.max() }
    check(result.winners == winners && result.pot == bets.sum())
    val expected =
        bets.indices.map {
            Rules.INITIAL_CHIPS - bets[it] +
                if (it in winners)
                    bets.sum() / winners.size +
                        if (it == winners.first()) bets.sum() % winners.size else 0
                else 0
        }
    check(result.seats.map { it.chips } == expected)
    check(result.seats.sumOf { it.chips } == Rules.INITIAL_CHIPS * 2)
}

private suspend fun view(client: Client): TableView {
    client.clear()
    client.send(TableViewRequest())
    return client.next<TableView>()
}

suspend fun scenario(id: String, nodes: Nodes) {
    val suffix = UUID.randomUUID().toString()
    val tableId = "table-$suffix"
    val p1 = Client(nodes.endpoint, "p1-$suffix", tableId)
    var p2 = Client(nodes.endpoint, "p2-$suffix", tableId)
    var primaryFailure: Exception? = null
    try {
        val first = p1.connect()
        val second = p2.connect()
        check(first.yourSeat == 0 && first.seats.single().chips == Rules.INITIAL_CHIPS)
        check(
            second.yourSeat == 1 &&
                second.seats.map { it.chips } == listOf(Rules.INITIAL_CHIPS, Rules.INITIAL_CHIPS)
        )
        val session1 = p1.sessionInfo()
        val session2 = p2.sessionInfo()
        p1.clear()
        p2.clear()
        p1.send(StartHandRequest())
        val cards1 = p1.next<HandDealt>()
        val cards2 = p2.next<HandDealt>()
        check(cards1.cards.size == Rules.CARDS && cards2.cards.size == Rules.CARDS)
        check((cards1.cards + cards2.cards).toSet().size == Rules.CARDS * 2)
        val start1 = p1.next<TableView> { it.inHand && it.turnSeat == 0 }
        p2.next<TableView> { it.inHand && it.turnSeat == 0 }
        if (id == "table") {
            p2.send(BetRequest(cards2.handNumber, 50))
            check(p2.next<ActionRejected>().reason == Rules.NOT_YOUR_TURN)
        }
        val bet1 = if (id == "table") 100 else 10
        p1.send(BetRequest(cards1.handNumber, bet1))
        val before1 = p1.next<TableView> { it.turnSeat == 1 && it.pot == bet1 }
        val before2 = p2.next<TableView> { it.turnSeat == 1 && it.pot == bet1 }
        assertChips(before1)
        when (id) {
            "reconnect" -> {
                p2.close()
                p2 = Client(nodes.endpoint, p2.playerId, tableId)
                val rejoined = p2.connect()
                val rejoinedSession = p2.sessionInfo()
                check(rejoinedSession.sessionId != session2.sessionId)
                check(rejoinedSession.actorId == session2.actorId)
                check(rejoinedSession.objectGeneration == session2.objectGeneration)
                same(before2, rejoined)
                check(p2.next<HandDealt>() == cards2)
                check(p1.sessionInfo() == session1 && p1.disconnects == 0 && p2.disconnects == 0)
            }
            "update" -> {
                nodes.drain(before1.nodeId)
                val after1 = view(p1)
                val after2 = view(p2)
                check(after1.nodeId != before1.nodeId && after2.nodeId == after1.nodeId)
                same(before1, after1)
                same(before2, after2)
                check(p1.next<HandDealt>() == cards1 && p2.next<HandDealt>() == cards2)
                check(p1.sessionInfo() == session1 && p2.sessionInfo() == session2)
                check(p1.disconnects == 0 && p2.disconnects == 0)
            }
        }
        // Only HandDealt may carry private cards before settlement; its recipient owns them.
        listOf(p1 to cards1, p2 to cards2).forEach { (player, dealt) ->
            synchronized(player.history) {
                player.history.forEach { message ->
                    when (message) {
                        is HandDealt -> check(message == dealt)
                        is TableView -> check(message.seats.all { it.cardCount in 0..Rules.CARDS })
                        is ActionRejected -> Unit
                        else -> error("Unexpected pre-settlement message: $message")
                    }
                }
            }
        }
        val bet2 = 50
        p2.send(BetRequest(cards2.handNumber, bet2))
        val result1 = p1.next<HandResult>()
        val result2 = p2.next<HandResult>()
        check(result1 == result2)
        result(result1, listOf(cards1.cards, cards2.cards), listOf(bet1, bet2))
        check(result1.handNumber == start1.handNumber)
        if (id == "update") {
            nodes.replace(before1.nodeId)
            scenario("table", nodes)
        }
    } catch (failure: Exception) {
        primaryFailure = failure
        throw failure
    } finally {
        var cleanupFailure: Exception? = null
        for (player in listOf(p2, p1)) {
            try {
                player.close()
            } catch (failure: Exception) {
                if (cleanupFailure == null) cleanupFailure = failure
                else cleanupFailure.addSuppressed(failure)
            }
        }
        if (cleanupFailure != null) {
            if (primaryFailure != null) primaryFailure.addSuppressed(cleanupFailure)
            else throw cleanupFailure
        }
    }
}
