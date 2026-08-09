package systems.zlink.samples.tictactoe.server.configuration;
import java.time.Duration;

public final class SampleNames {
    public static final String ApiChannel = "tictactoe-api";
    public static final String SpotMesh = "tictactoe";
    public static final String PlayNode = "play";
    public static final String PlayerMilestoneTopic = "tictactoe.player.milestone";
    public static final String PlayStream = "play-stream";
    public static final String PlayActor = "play-actor";
    public static final int RequiredLevel = 3;
    public static final Duration RequestTimeout = Duration.ofSeconds(5);

    private SampleNames() {
    }

}
