package systems.zlink.samples.tictactoe.server.configuration;

public final class SampleConfigPath {
    private SampleConfigPath() {
    }

    public static String require(String[] args) {
        if (args.length != 2 || !"--config".equals(args[0]) || args[1].isBlank()) {
            throw new IllegalArgumentException("Usage: <role executable> --config <path>");
        }
        return args[1];
    }
}
