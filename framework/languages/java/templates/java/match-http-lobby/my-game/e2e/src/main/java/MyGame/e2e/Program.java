package MyGame.e2e;

import MyGame.*;

import java.nio.file.Path;
import java.util.*;

public final class Program {
    private Program() {}

    public static void main(String[] args) throws Exception {
        boolean attach = args.length > 0 && args[0].equals("--attach");
        if (attach) args = Arrays.copyOfRange(args, 1, args.length);
        var root = Path.of("").toAbsolutePath().normalize();
        Structure.check(root);
        if (Arrays.equals(args, new String[] {"--check-structure"})) return;
        LobbyDomainChecks.run();
        if (Arrays.equals(args, new String[] {"--check-domain"})) return;
        Structure.require(
                args.length == 0 || (args.length == 2 && args[0].equals("--scenario")),
                "Use --scenario a,b");
        var requested =
                (args.length == 0 ? Names.SCENARIOS : List.of(args[1].split(",", -1)))
                        .stream().map(Scenario::parse).toList();
        Structure.require(
                !requested.isEmpty() && new HashSet<>(requested).size() == requested.size(),
                "Invalid scenarios");
        var selected = Arrays.stream(Scenario.values()).filter(requested::contains).toList();
        var state = attach ? ServerHost.connection(root) : null;
        Structure.require(
                !attach || state != null, "The running server stopped before e2e could attach.");
        var scenarios = new ArrayList<>(selected);
        if (attach) {
            for (var scenario : selected) {
                String reason =
                        switch (scenario) {
                            case UPDATE -> "requires draining and replacing a play node";
                            case LOBBY -> "requires restarting the lobby API node";
                            default -> null;
                        };
                if (reason != null) {
                    scenarios.remove(scenario);
                    System.out.printf(
                            "Skipping %s: %s (isolated server required).%n", scenario.id(), reason);
                }
            }
        }
        var logs = RunLogs.create(root);
        int passed = 0;
        for (int i = 0; i < scenarios.size(); i++) {
            var scenario = scenarios.get(i);
            RunLogs.heading(scenario.id(), i + 1, scenarios.size());
            var scenarioLogs = attach ? logs : logs.resolve(scenario.id());
            try (var nodes = new Nodes(root, scenarioLogs)) {
                if (attach) nodes.attach(state);
                else nodes.start();
                Scenarios.run(scenario, nodes);
            } catch (Exception error) {
                RunLogs.failure(logs, scenario.id(), error);
                continue;
            }
            passed++;
            System.out.println("✓ " + scenario.id());
        }
        printSummary(passed, scenarios.size());
        if (passed != scenarios.size()) System.exit(1);
    }

    private static void printSummary(int passed, int total) {
        System.out.println("Scenarios: " + passed + "/" + total + " passed");
    }
}
