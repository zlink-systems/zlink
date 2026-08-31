package systems.zlink.samples.tictactoe.client;

public record TicTacToeClientOptions(
    String apiUrl,
    String gameName,
    String xActorId,
    String oActorId,
    String observerActorId,
    String lifecycleCompletionFile) {
    public static TicTacToeClientOptions createDefault() {
        return new TicTacToeClientOptions(
            TicTacToeSampleDefaults.ApiUrl,
            TicTacToeSampleDefaults.GameName,
            TicTacToeSampleDefaults.XActorId,
            TicTacToeSampleDefaults.OActorId,
            TicTacToeSampleDefaults.ObserverActorId,
            "");
    }
}
