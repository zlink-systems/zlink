package MyGame.e2e;

import MyGame.*;

import java.nio.file.Path;
import java.util.*;

public final class Program {
    private Program() {}

    public static void main(String[] args) throws Exception {
        var root = Path.of("").toAbsolutePath().normalize();
        Structure.check(root);
        if (Arrays.equals(args, new String[] {"--check-structure"})) return;
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
        int passed = 0;
        for (var scenario : selected) {
            try (var nodes = new Nodes(root, scenario == Scenario.LOBBY ? 3 : 1)) {
                System.out.println("Logs: " + nodes.logs);
                nodes.start();
                Scenarios.run(scenario, nodes);
            } catch (Exception error) {
                System.out.println("✗ " + scenario.id());
                error.printStackTrace(System.err);
                System.out.println("Scenarios: " + passed + "/" + selected.size() + " passed");
                System.exit(1);
            }
            passed++;
            System.out.println("✓ " + scenario.id());
        }
        System.out.println("Scenarios: " + passed + "/" + selected.size() + " passed");
    }
}
