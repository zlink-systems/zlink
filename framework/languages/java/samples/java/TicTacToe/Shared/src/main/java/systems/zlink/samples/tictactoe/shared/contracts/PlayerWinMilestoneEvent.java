package systems.zlink.samples.tictactoe.shared.contracts;

public record PlayerWinMilestoneEvent(
    String roomId,
    String actorId,
    String displayName,
    int wins) {
}
