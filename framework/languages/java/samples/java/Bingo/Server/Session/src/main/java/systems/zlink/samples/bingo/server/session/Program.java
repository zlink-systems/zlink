package systems.zlink.samples.bingo.server.session;

import systems.zlink.samples.bingo.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        SessionServerApplication.run(SampleTopology.configPath(args));
    }
}
