package MyGame.e2e

import MyGame.*

internal suspend fun matchScenario(endpoint: String, cancelFirst: Boolean) {
    if (cancelFirst) {
        val client = Client(endpoint, "", "")
        withCleanup(cleanup = { client.close() }) {
            client.connectForMatch()
            val queued = client.next<MatchQueued>()
            client.send(FindMatch())
            check(client.next<MatchQueued>() == queued)
            client.send(CancelMatch(queued.ticketId))
            check(client.next<MatchCancelled>() == MatchCancelled(queued.ticketId, "cancelled"))
        }
    }

    val first = Client(endpoint, "", "")
    val second = Client(endpoint, "", "")
    withCleanup(cleanup = { withCleanup(cleanup = { first.close() }) { second.close() } }) {
        first.connectForMatch()
        val firstTicket = first.next<MatchQueued>().ticketId
        second.connectForMatch()
        val secondTicket = second.next<MatchQueued>().ticketId
        check(firstTicket.isNotBlank() && secondTicket.isNotBlank() && firstTicket != secondTicket)
        val firstMatch = first.next<MatchFound>()
        val secondMatch = second.next<MatchFound>()
        check(firstMatch.ticketId == firstTicket && secondMatch.ticketId == secondTicket)
        check(firstMatch.roomId.startsWith("room-") && firstMatch.roomId == secondMatch.roomId)
        check(setOf(firstMatch.seat, secondMatch.seat) == setOf(0, 1))
        val clients = listOf(first to firstMatch, second to secondMatch).sortedBy { it.second.seat }
        for ((client, matched) in clients) {
            val view = client.next<TableView>()
            check(view.tableId == matched.roomId && view.yourSeat == matched.seat)
            check(view.seats.map { it.chips } == listOf(Rules.INITIAL_CHIPS, Rules.INITIAL_CHIPS))
            synchronized(client.history) {
                check(
                    client.history.indexOfFirst { it is MatchQueued } <
                        client.history.indexOf(matched)
                )
                check(client.history.indexOf(matched) < client.history.indexOf(view))
            }
        }
        if (!cancelFirst) settleMatchedHand(clients.map { it.first })
    }
}

private suspend fun settleMatchedHand(clients: List<Client>) {
    val (first, second) = clients
    first.send(StartHandRequest())
    val cards = clients.map { it.next<HandDealt>() }
    check(cards.all { it.cards.size == Rules.CARDS })
    check(cards.flatMap { it.cards }.toSet().size == Rules.CARDS * 2)
    val bets = listOf(25L, 50L)
    first.send(BetRequest(cards[0].handNumber, bets[0]))
    second.next<TableView> { it.turnSeat == 1 && it.pot == bets[0] }
    second.send(BetRequest(cards[1].handNumber, bets[1]))
    val settled = clients.map { it.next<HandResult>() }
    check(settled[0] == settled[1])
    result(settled[0], cards.map { it.cards }, bets)
}
