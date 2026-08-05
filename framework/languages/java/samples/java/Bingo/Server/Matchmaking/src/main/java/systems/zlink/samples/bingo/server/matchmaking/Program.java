package systems.zlink.samples.bingo.server.matchmaking;

import systems.zlink.samples.bingo.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        MatchmakingServerApplication.run(SampleTopology.configPath(args));
    }
}
