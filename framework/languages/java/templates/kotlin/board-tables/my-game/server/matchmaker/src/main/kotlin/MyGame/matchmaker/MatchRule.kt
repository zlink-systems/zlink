package MyGame.matchmaker

internal object MatchRule {
    fun select(waiting: Sequence<Ticket>): List<Ticket> = waiting.take(2).toList()
}
