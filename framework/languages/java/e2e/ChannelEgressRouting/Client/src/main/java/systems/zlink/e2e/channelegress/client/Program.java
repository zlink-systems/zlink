package systems.zlink.e2e.channelegress.client;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        if (args.length != 4 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()) {
            throw new IllegalArgumentException(
                "Usage: channel-egress-client --config <path> --scenario <selector>");
        }
        ScenarioSuite.run(args[3], ClientOptions.load(args[1]));
        System.out.println("channel-egress-routing result=passed");
    }
}
