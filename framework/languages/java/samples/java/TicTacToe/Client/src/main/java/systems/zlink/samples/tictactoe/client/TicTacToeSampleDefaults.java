package systems.zlink.samples.tictactoe.client;

public final class TicTacToeSampleDefaults {
    public static final String ApiUrl = "http://127.0.0.1:18080";
    public static final String GameName = "tictactoe-game";
    public static final String XActorId = "player-x";
    public static final String OActorId = "player-o";
    public static final String ObserverActorId = "player-observer";
    public static final java.time.Duration RequestTimeout = java.time.Duration.ofSeconds(10);

    private TicTacToeSampleDefaults() {
    }
}
