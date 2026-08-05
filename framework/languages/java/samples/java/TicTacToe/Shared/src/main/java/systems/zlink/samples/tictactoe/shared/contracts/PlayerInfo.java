package systems.zlink.samples.tictactoe.shared.contracts;

public record PlayerInfo(
    String actorId,
    String displayName,
    int level,
    int wins) {
}
