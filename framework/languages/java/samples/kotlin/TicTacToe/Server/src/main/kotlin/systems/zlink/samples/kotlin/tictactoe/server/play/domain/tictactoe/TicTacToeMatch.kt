package systems.zlink.samples.kotlin.tictactoe.server.play.domain.tictactoe

import java.time.Duration
import java.time.Instant
import systems.zlink.samples.kotlin.tictactoe.shared.contracts.GameState

class TicTacToeMatch(
    private val roomId: String,
    private val turnTimeout: Duration,
) {
    private val board = TicTacToeBoard()
    private val players = linkedMapOf<String, String>()
    private var status = "WaitingForPlayers"
    private var winner: String? = null
    private var nextTurn = "X"
    private var lastMoveActorId: String? = null
    private var lastMoveCell: Int? = null
    private var turnDeadline: Instant? = null

    fun joinPlayer(actorId: String, now: Instant): JoinChange {
        val existingMark = players[actorId]
        if (existingMark != null) {
            return JoinChange(snapshot(), existingMark, isNewPlayer = false)
        }

        val mark = when (players.size) {
            0 -> "X"
            1 -> "O"
            else -> throw IllegalStateException("tic-tac-toe game already has two players")
        }

        players[actorId] = mark
        if (players.size == 2 && status == "WaitingForPlayers") {
            status = "InProgress"
            resetTurnDeadline(now)
        }
        return JoinChange(snapshot(), mark, isNewPlayer = true)
    }

    fun previewJoin(actorId: String): JoinChange {
        val existingMark = players[actorId]
        if (existingMark != null) {
            return JoinChange(snapshot(), existingMark, isNewPlayer = false)
        }
        check(players.size < 2) { "tic-tac-toe game already has two players" }

        val mark = if (players.isEmpty()) "X" else "O"
        val state = GameState(
            roomId = roomId,
            board = board.snapshot(),
            status = if (players.size == 1) "InProgress" else status,
            winner = winner,
            nextTurn = nextTurn,
            xActorId = if (mark == "X") actorId else players.entries.firstOrNull { it.value == "X" }?.key,
            oActorId = if (mark == "O") actorId else players.entries.firstOrNull { it.value == "O" }?.key,
            lastMoveActorId = lastMoveActorId,
            lastMoveCell = lastMoveCell,
        )
        return JoinChange(state, mark, isNewPlayer = true)
    }

    fun placeMark(actorId: String, cell: Int, now: Instant): MoveChange {
        val mark = players[actorId] ?: throw IllegalStateException("player has not joined")
        check(status == "InProgress") { "game is not in progress" }
        check(mark == nextTurn) { "unexpected turn" }

        val before = snapshot()
        board.place(mark, cell)
        lastMoveActorId = actorId
        lastMoveCell = cell
        advanceAfterMove(actorId, mark, now)
        return MoveChange(before, snapshot())
    }

    fun tick(now: Instant): TickChange {
        val deadline = turnDeadline
        if (status != "InProgress" || deadline == null || now.isBefore(deadline)) {
            return TickChange(snapshot(), changed = false)
        }

        val timedOut = players.entries.firstOrNull { it.value == nextTurn }?.key
        val timeoutWinner = players.entries.firstOrNull { it.value != nextTurn }?.key

        status = "TurnTimedOut"
        winner = timeoutWinner
        nextTurn = ""
        lastMoveActorId = timedOut
        lastMoveCell = null
        turnDeadline = null
        return TickChange(snapshot(), changed = true)
    }

    fun snapshot(): GameState =
        GameState(
            roomId = roomId,
            board = board.snapshot(),
            status = status,
            winner = winner,
            nextTurn = nextTurn,
            xActorId = players.entries.firstOrNull { it.value == "X" }?.key,
            oActorId = players.entries.firstOrNull { it.value == "O" }?.key,
            lastMoveActorId = lastMoveActorId,
            lastMoveCell = lastMoveCell,
        )

    private fun advanceAfterMove(actorId: String, mark: String, now: Instant) {
        if (board.hasWon(mark)) {
            status = "Won"
            winner = actorId
            nextTurn = ""
            turnDeadline = null
            return
        }

        if (board.isFull()) {
            status = "Draw"
            winner = null
            nextTurn = ""
            turnDeadline = null
            return
        }

        nextTurn = if (mark == "X") "O" else "X"
        resetTurnDeadline(now)
    }

    private fun resetTurnDeadline(now: Instant) {
        turnDeadline = now.plus(turnTimeout)
    }

    data class JoinChange(
        val state: GameState,
        val mark: String,
        val isNewPlayer: Boolean,
    )

    data class MoveChange(
        val before: GameState,
        val after: GameState,
    )

    data class TickChange(
        val state: GameState,
        val changed: Boolean,
    )
}

private class TicTacToeBoard {
    private val cells = CharArray(9) { '.' }

    fun snapshot(): String = String(cells)

    fun place(mark: String, cell: Int) {
        require(cell in cells.indices && cells[cell] == '.') { "invalid cell" }
        cells[cell] = mark[0]
    }

    fun isFull(): Boolean = cells.none { it == '.' }

    fun hasWon(mark: String): Boolean {
        val symbol = mark[0]
        return WinningLines.any { (a, b, c) ->
            cells[a] == symbol && cells[b] == symbol && cells[c] == symbol
        }
    }

    private companion object {
        private val WinningLines = arrayOf(
            intArrayOf(0, 1, 2),
            intArrayOf(3, 4, 5),
            intArrayOf(6, 7, 8),
            intArrayOf(0, 3, 6),
            intArrayOf(1, 4, 7),
            intArrayOf(2, 5, 8),
            intArrayOf(0, 4, 8),
            intArrayOf(2, 4, 6),
        )
    }
}
