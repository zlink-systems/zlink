package systems.zlink.samples.tictactoe.client;

public final class TicTacToeClientArguments {
    private TicTacToeClientArguments() {
    }

    public static TicTacToeClientOptions parse(String[] args) {
        TicTacToeClientOptions defaults = TicTacToeClientOptions.createDefault();
        return new TicTacToeClientOptions(
            readOption(args, "--api-url", defaults.apiUrl()),
            readOption(args, "--game-name", defaults.gameName()),
            readOption(args, "--x-actor-id", defaults.xActorId()),
            readOption(args, "--o-actor-id", defaults.oActorId()),
            readOption(args, "--observer-actor-id", defaults.observerActorId()));
    }

    private static String readOption(
        String[] args,
        String name,
        String defaultValue) {
        for (int index = 0; index < args.length; index++) {
            if (!args[index].equals(name)) {
                continue;
            }
            if (index + 1 >= args.length) {
                throw new IllegalArgumentException("Missing value for '" + name + "'.");
            }
            return args[index + 1];
        }
        return defaultValue;
    }
}
