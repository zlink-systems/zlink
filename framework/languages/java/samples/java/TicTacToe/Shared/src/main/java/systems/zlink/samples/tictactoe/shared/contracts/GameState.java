package systems.zlink.samples.tictactoe.shared.contracts;

public record GameState(
    String roomId,
    String board,
    String status,
    String winner,
    String nextTurn,
    String xActorId,
    String oActorId,
    String lastMoveActorId,
    Integer lastMoveCell) {
}
