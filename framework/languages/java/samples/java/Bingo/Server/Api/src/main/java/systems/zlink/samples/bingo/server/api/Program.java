package systems.zlink.samples.bingo.server.api;

import systems.zlink.samples.bingo.server.configuration.SampleTopology;

public final class Program {
    private Program() {
    }

    public static void main(String[] args) {
        ApiServerApplication.run(SampleTopology.configPath(args));
    }
}
