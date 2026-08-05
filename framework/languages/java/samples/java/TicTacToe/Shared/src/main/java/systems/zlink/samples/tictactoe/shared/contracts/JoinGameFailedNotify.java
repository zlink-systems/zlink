package systems.zlink.samples.tictactoe.shared.contracts;

public record JoinGameFailedNotify(
    String roomId,
    String reason,
    boolean retriable) {
}
