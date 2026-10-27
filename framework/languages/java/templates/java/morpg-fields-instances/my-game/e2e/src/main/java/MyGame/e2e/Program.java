package MyGame.e2e;

import MyGame.*;

import java.nio.file.Path;
import java.util.*;

public final class Program {
    private Program() {}

    public static void main(String[] args) throws Exception {
        var root = Path.of("").toAbsolutePath().normalize();
        if (Arrays.equals(args, new String[] {"--check-structure"})) {
            Structure.check(root);
            return;
        }
        DomainChecks.run();
        if (Arrays.equals(args, new String[] {"--check-domain"})) return;
        Structure.check(root);
        Structure.require(
                args.length == 0 || (args.length == 2 && args[0].equals("--scenario")),
                "Use --scenario a,b");
        var requested =
                (args.length == 0 ? Names.SCENARIOS : List.of(args[1].split(",", -1)))
                        .stream().map(String::trim).map(Scenario::parse).toList();
        Structure.require(
                !requested.isEmpty() && new HashSet<>(requested).size() == requested.size(),
                "Invalid scenarios");
        var selected = Arrays.stream(Scenario.values()).filter(requested::contains).toList();
        int passed = 0;
        boolean cleanupFailed = false;
        var nodes = new Nodes(root);
        try {
            System.out.println("Logs: " + nodes.logs);
            nodes.start();
            try (var scenarios = new Scenarios(nodes, selected)) {
                Exception dependencyFailure = null;
                for (var id : selected) {
                    if (id != Scenario.FIELD && dependencyFailure != null) {
                        System.out.println(
                                "✗ "
                                        + id.id()
                                        + " (blocked by party fixture: "
                                        + dependencyFailure.getMessage()
                                        + ")");
                        continue;
                    }
                    try {
                        scenarios.run(id);
                        passed++;
                        System.out.println("✓ " + id.id());
                    } catch (Exception error) {
                        if (id != Scenario.FIELD) dependencyFailure = error;
                        System.out.println("✗ " + id.id());
                        error.printStackTrace(System.err);
                    }
                }
            }
        } catch (Exception error) {
            cleanupFailed = true;
            error.printStackTrace(System.err);
        } finally {
            try {
                nodes.close();
            } catch (Exception error) {
                cleanupFailed = true;
                error.printStackTrace(System.err);
            }
        }
        System.out.println("Scenarios: " + passed + "/" + selected.size() + " passed");
        System.exit(passed == selected.size() && !cleanupFailed ? 0 : 1);
    }
}
