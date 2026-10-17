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
            passed = Scenarios.run(selected, nodes);
        } catch (Exception error) {
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
