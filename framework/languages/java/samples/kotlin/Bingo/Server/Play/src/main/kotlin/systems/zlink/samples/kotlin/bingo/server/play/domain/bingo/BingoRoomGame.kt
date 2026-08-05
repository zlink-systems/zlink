package systems.zlink.samples.kotlin.bingo.server.play.domain.bingo

import java.util.ArrayDeque
import systems.zlink.samples.kotlin.bingo.shared.contracts.BingoRoomState
import systems.zlink.samples.kotlin.bingo.shared.contracts.card
import systems.zlink.samples.kotlin.bingo.shared.contracts.drawnNumbers
import systems.zlink.samples.kotlin.bingo.shared.contracts.marks
import systems.zlink.samples.kotlin.bingo.shared.contracts.players
import systems.zlink.samples.kotlin.bingo.shared.contracts.winners

class BingoRoomGame(
    private val roomId: String,
    private val settings: BingoRoomSettings,
) {
    private val players = mutableListOf<BingoRoomPlayer>()
    private val drawDeck = ArrayDeque<Int>()
    private val drawnNumbers = mutableListOf<Int>()
    private val winners = mutableListOf<String>()
    private var status: String = WaitingForPlayers

    init {
        for (number in 1..settings.maxDrawNumber) {
            drawDeck.add(number)
        }
    }

    fun join(
        actorId: String,
        displayName: String,
        wins: Int,
        losses: Int,
    ): Change {
        val existing = player(actorId)
        if (existing != null) {
            return Change(snapshot(), emptyList(), false)
        }
        if (status != WaitingForPlayers || players.size >= settings.requiredPlayers) {
            throw IllegalStateException("Room $roomId cannot accept more players.")
        }

        val joined = BingoRoomPlayer(actorId, displayName, players.size, null, wins, losses)
        players += joined
        val joinedState = snapshot()
        val events = playerJoinedEvents(joined, joinedState).toMutableList()
        if (players.size == settings.requiredPlayers) {
            status = Running
            val startedState = snapshot()
            events += eventsForAll(BingoRoomEventKind.GAME_STARTED, startedState)
            return Change(startedState, events, true)
        }
        return Change(joinedState, events, false)
    }

    fun previewJoin(actorId: String, displayName: String): BingoRoomState {
        val existing = player(actorId)
        if (existing != null) {
            return snapshot()
        }
        if (status != WaitingForPlayers || players.size >= settings.requiredPlayers) {
            throw IllegalStateException("Room $roomId cannot accept more players.")
        }
        val previewPlayers = players + BingoRoomPlayer(actorId, displayName, players.size, null, 0, 0)
        val previewStatus = if (previewPlayers.size == settings.requiredPlayers) Running else status
        return snapshot(previewPlayers, previewStatus)
    }

    fun submitCard(
        actorId: String,
        submittedCard: List<Int>,
    ): Change {
        val index = playerIndex(actorId)
        if (status != Running) {
            throw IllegalStateException("bingo card can be submitted only after the room starts")
        }
        val current = players[index]
        if (current.card != null) {
            throw IllegalStateException("bingo card was already submitted")
        }
        players[index] = BingoRoomPlayer(
            current.actorId,
            current.displayName,
            current.seat,
            BingoCard.from(submittedCard),
            current.wins,
            current.losses,
        )
        return Change(snapshot(), emptyList(), allCardsSubmitted())
    }

    fun drawNext(): Change {
        if (status != Running || !allCardsSubmitted() || drawDeck.isEmpty()) {
            return Change(snapshot(), emptyList(), allCardsSubmitted())
        }

        val number = drawDeck.remove()
        drawnNumbers += number
        val newlyCompleted = mutableListOf<String>()
        for (player in players) {
            val card = player.card ?: continue
            val before = card.completedLines()
            card.markDrawnNumber(number)
            if (card.completedLines() > before) {
                newlyCompleted += player.actorId
            }
        }

        if (newlyCompleted.isNotEmpty()) {
            status = Finished
            winners += newlyCompleted
        }

        val state = snapshot()
        val events = numberDrawnEvents(state, number).toMutableList()
        events += eventsForAll(
            if (status == Finished) BingoRoomEventKind.GAME_ENDED else BingoRoomEventKind.STATE,
            state,
        )
        return Change(state, events, false)
    }

    fun snapshot(): BingoRoomState = snapshot(players, status)

    private fun snapshot(
        snapshotPlayers: List<BingoRoomPlayer>,
        snapshotStatus: String,
    ): BingoRoomState {
        val hostActorId = snapshotPlayers.firstOrNull()?.actorId ?: ""
        val lastDrawn = drawnNumbers.lastOrNull()
        return BingoRoomState(
            roomId,
            snapshotStatus,
            hostActorId,
            false,
            drawnNumbers.size,
            lastDrawn,
            drawnNumbers.toList(),
            snapshotPlayers.map { it.toState(hostActorId) },
            winners.toList(),
        )
    }

    private fun allCardsSubmitted(): Boolean =
        players.size == settings.requiredPlayers && players.all { it.card != null }

    private fun player(actorId: String): BingoRoomPlayer? =
        players.firstOrNull { it.actorId == actorId }

    private fun playerIndex(actorId: String): Int {
        players.forEachIndexed { index, player ->
            if (player.actorId == actorId) {
                return index
            }
        }
        throw IllegalStateException("player has not joined the bingo room")
    }

    private fun playerJoinedEvents(
        joined: BingoRoomPlayer,
        state: BingoRoomState,
    ): List<BingoRoomEvent> =
        players.mapNotNull { player ->
            if (player.actorId == joined.actorId) {
                return@mapNotNull null
            }
            BingoRoomEvent(
                BingoRoomEventKind.PLAYER_JOINED,
                player.actorId,
                state,
                joined.actorId,
                joined.displayName,
                joined.seat,
                joined.seat == 0,
                0,
            )
        }

    private fun numberDrawnEvents(
        state: BingoRoomState,
        number: Int,
    ): List<BingoRoomEvent> =
        players.map { player ->
            BingoRoomEvent(
                BingoRoomEventKind.NUMBER_DRAWN,
                player.actorId,
                state,
                null,
                null,
                -1,
                false,
                number,
            )
        }

    private fun eventsForAll(
        kind: BingoRoomEventKind,
        state: BingoRoomState,
    ): List<BingoRoomEvent> =
        players.map { player ->
            BingoRoomEvent(
                kind,
                player.actorId,
                state,
                null,
                null,
                -1,
                false,
                0,
            )
        }

    data class Change(
        val state: BingoRoomState,
        val events: List<BingoRoomEvent>,
        val drawReady: Boolean,
    )

    companion object {
        const val WaitingForPlayers = "WaitingForPlayers"
        const val Running = "Running"
        const val Finished = "Finished"

        fun restore(
            roomId: String,
            settings: BingoRoomSettings,
            state: BingoRoomState,
        ): BingoRoomGame = BingoRoomGame(roomId, settings).also { game ->
            game.status = state.status
            game.drawnNumbers += state.drawnNumbers
            game.drawDeck.removeAll(state.drawnNumbers.toSet())
            game.winners += state.winners
            state.players.forEach { player ->
                game.players += BingoRoomPlayer(
                    actorId = player.actorId,
                    displayName = player.displayName,
                    seat = player.seat,
                    card = player.card.takeIf { it.isNotEmpty() }
                        ?.let { BingoCard.restore(it, player.marks) },
                    wins = player.wins,
                    losses = player.losses,
                )
            }
        }
    }
}
