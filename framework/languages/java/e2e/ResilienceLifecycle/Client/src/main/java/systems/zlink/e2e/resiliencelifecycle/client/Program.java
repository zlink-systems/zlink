package systems.zlink.e2e.resiliencelifecycle.client;

import java.util.List;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ClientOptions;
import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceProcessManager;

public final class Program {
    private Program() {
    }

    public static void main(String... args) {
        Inputs inputs = parseInputs(args);
        ClientOptions options = ClientOptions.load(inputs.configPath());
        try (ResilienceProcessManager processes = new ResilienceProcessManager(options)) {
            new ResilienceLifecycleSuite(options, processes, inputs.startOrder())
                .run(inputs.scenario());
        }
        System.out.println("resilience-lifecycle e2e result=passed");
    }

    private static Inputs parseInputs(String[] args) {
        if (args.length != 6 || !"--config".equals(args[0]) || args[1].isBlank()
            || !"--scenario".equals(args[2]) || args[3].isBlank()
            || !"--start-order".equals(args[4])) {
            throw new IllegalArgumentException("Usage: Client --config <path> --scenario <selector> --start-order <api-a,api-b>");
        }
        List<String> roles = List.of(args[5].split(",", -1));
        if (roles.size() != 2 || !roles.contains("api-a") || !roles.contains("api-b")) {
            throw new IllegalArgumentException("start order must contain api-a and api-b exactly once");
        }
        return new Inputs(args[1], args[3], roles);
    }

    private record Inputs(String configPath, String scenario, List<String> startOrder) { }
}
