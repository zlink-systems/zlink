package systems.zlink.samples.tictactoe.server.play;

import systems.zlink.samples.tictactoe.server.configuration.SampleConfigPath;

public final class PlayProgram {
    private PlayProgram() {
    }

    public static void main(String[] args) {
        PlayServerApplication.run(SampleConfigPath.require(args));
    }
}
