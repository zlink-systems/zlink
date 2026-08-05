package systems.zlink.samples.tictactoe.server.play.domain.tictactoe;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import systems.zlink.samples.tictactoe.shared.contracts.GameState;

public final class TicTacToeMatch {
    private final String roomId;
    private final TicTacToeBoard board = new TicTacToeBoard();
    private final List<PlayerSlot> players = new ArrayList<>();
    private String status = "WaitingForPlayers";
    private String nextTurn = "X";
    private String winner = "";
    private String lastMoveActorId = "";
    private int lastMoveCell = -1;
    private Instant turnDeadline;

    public TicTacToeMatch(String roomId) {
        this.roomId = roomId;
    }

    public JoinResult join(String actorId, Instant now, Duration turnTimeout) {
        PlayerSlot slot = players.stream()
            .filter(player -> player.actorId().equals(actorId))
            .findFirst()
            .orElse(null);
        boolean newlyJoined = slot == null;
        if (slot == null) {
            if (players.size() >= 2) {
                throw new IllegalStateException("tic-tac-toe game already has two players");
            }
            slot = new PlayerSlot(actorId, players.isEmpty() ? "X" : "O");
            players.add(slot);
        }
        if (players.size() == 2 && status.equals("WaitingForPlayers")) {
            status = "InProgress";
            resetTurnDeadline(now, turnTimeout);
        }
        return new JoinResult(newlyJoined, slot.mark(), snapshot());
    }

    public JoinResult previewJoin(String actorId) {
        PlayerSlot existing = players.stream()
            .filter(player -> player.actorId().equals(actorId))
            .findFirst()
            .orElse(null);
        if (existing != null) {
            return new JoinResult(false, existing.mark(), snapshot());
        }
        if (players.size() >= 2) {
            throw new IllegalStateException("tic-tac-toe game already has two players");
        }

        String mark = players.isEmpty() ? "X" : "O";
        String xActorId = mark.equals("X") ? actorId : actorIdForMark("X");
        String oActorId = mark.equals("O") ? actorId : actorIdForMark("O");
        GameState state = new GameState(
            roomId,
            board.snapshot(),
            players.size() == 1 ? "InProgress" : status,
            winner.isEmpty() ? null : winner,
            nextTurn,
            xActorId,
            oActorId,
            lastMoveActorId.isEmpty() ? null : lastMoveActorId,
            lastMoveCell < 0 ? null : lastMoveCell);
        return new JoinResult(true, mark, state);
    }

    public boolean canJoin(String actorId) {
        return players.size() < 2 || players.stream()
            .anyMatch(player -> player.actorId().equals(actorId));
    }

    public GameState placeMark(String actorId, int cell, Instant now, Duration turnTimeout) {
        PlayerSlot slot = player(actorId);
        if (!status.equals("InProgress")) {
            throw new IllegalStateException("game is not in progress");
        }
        if (!slot.mark().equals(nextTurn)) {
            throw new IllegalStateException("unexpected turn");
        }

        board.place(cell, slot.mark());
        lastMoveActorId = actorId;
        lastMoveCell = cell;
        advance(slot, now, turnTimeout);
        return snapshot();
    }

    public GameState timeOutCurrentTurn(Instant now) {
        if (!status.equals("InProgress")
            || turnDeadline == null
            || now.isBefore(turnDeadline)) {
            return null;
        }

        PlayerSlot timedOut = players.stream()
            .filter(player -> player.mark().equals(nextTurn))
            .findFirst()
            .orElse(null);
        PlayerSlot winningSlot = players.stream()
            .filter(player -> !player.mark().equals(nextTurn))
            .findFirst()
            .orElse(null);

        status = "TurnTimedOut";
        winner = winningSlot == null ? "" : winningSlot.actorId();
        nextTurn = "";
        lastMoveActorId = timedOut == null ? "" : timedOut.actorId();
        lastMoveCell = -1;
        turnDeadline = null;
        return snapshot();
    }

    public GameState snapshot() {
        return new GameState(
            roomId,
            board.snapshot(),
            status,
            winner.isEmpty() ? null : winner,
            nextTurn,
            actorIdForMark("X"),
            actorIdForMark("O"),
            lastMoveActorId.isEmpty() ? null : lastMoveActorId,
            lastMoveCell < 0 ? null : lastMoveCell);
    }

    private void advance(PlayerSlot slot, Instant now, Duration turnTimeout) {
        if (board.hasWon(slot.mark())) {
            status = "Won";
            winner = slot.actorId();
            nextTurn = "";
            turnDeadline = null;
        } else if (board.isFull()) {
            status = "Draw";
            winner = "";
            nextTurn = "";
            turnDeadline = null;
        } else {
            nextTurn = slot.mark().equals("X") ? "O" : "X";
            resetTurnDeadline(now, turnTimeout);
        }
    }

    private void resetTurnDeadline(Instant now, Duration turnTimeout) {
        turnDeadline = now.plus(turnTimeout);
    }

    private PlayerSlot player(String actorId) {
        return players.stream()
            .filter(player -> player.actorId().equals(actorId))
            .findFirst()
            .orElseThrow(() -> new IllegalStateException("player has not joined"));
    }

    private String actorIdForMark(String mark) {
        return players.stream()
            .filter(player -> player.mark().equals(mark))
            .map(PlayerSlot::actorId)
            .findFirst()
            .orElse(null);
    }

    public record JoinResult(boolean newlyJoined, String mark, GameState state) {
    }

    private record PlayerSlot(String actorId, String mark) {
    }
}
