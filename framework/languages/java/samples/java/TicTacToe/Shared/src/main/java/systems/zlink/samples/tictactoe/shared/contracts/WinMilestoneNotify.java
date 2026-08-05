package systems.zlink.samples.tictactoe.shared.contracts;

public record WinMilestoneNotify(
    String roomId,
    String actorId,
    String displayName,
    int wins) {
}
