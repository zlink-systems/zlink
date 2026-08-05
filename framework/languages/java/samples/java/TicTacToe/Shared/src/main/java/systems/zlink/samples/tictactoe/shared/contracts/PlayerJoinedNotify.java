package systems.zlink.samples.tictactoe.shared.contracts;

public record PlayerJoinedNotify(
    String roomId,
    String actorId,
    String displayName,
    int level,
    String mark,
    GameState state) {
}
