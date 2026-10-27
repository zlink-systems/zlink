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
        var selected =
                (args.length == 0 ? Names.SCENARIOS : List.of(args[1].split(",", -1)))
                        .stream().map(Scenario::parse).toList();
        Structure.require(
                !selected.isEmpty() && new HashSet<>(selected).size() == selected.size(),
                "Invalid scenarios");
        int passed = 0;
        for (var id : Arrays.stream(Scenario.values()).filter(selected::contains).toList()) {
            try (var nodes = new Nodes(root)) {
                System.out.println("Logs: " + nodes.logs);
                nodes.start();
                Scenarios.run(id, nodes);

            } catch (Exception error) {
                System.out.println("✗ " + id.id());
                error.printStackTrace(System.err);
                continue;
            }
            passed++;
            System.out.println("✓ " + id.id());
        }
        System.out.println("Scenarios: " + passed + "/" + selected.size() + " passed");
        System.exit(passed == selected.size() ? 0 : 1);
    }
}
