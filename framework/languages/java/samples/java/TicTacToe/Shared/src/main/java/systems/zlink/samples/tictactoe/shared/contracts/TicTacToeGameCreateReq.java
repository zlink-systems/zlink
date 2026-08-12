package systems.zlink.samples.tictactoe.shared.contracts;

public record TicTacToeGameCreateReq(
    String gameName,
    int requiredLevel) {
}
