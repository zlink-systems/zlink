package systems.zlink.samples.tictactoe.shared.contracts;

public record PlayerWinMilestoneMsg(
    String roomId,
    String actorId,
    String displayName,
    int wins) {
}
