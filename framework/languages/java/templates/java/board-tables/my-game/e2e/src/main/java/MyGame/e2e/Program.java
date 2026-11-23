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
        boolean attach = args.length > 0 && args[0].equals("--attach");
        var scenarioArgs = Arrays.copyOfRange(args, attach ? 1 : 0, args.length);
        Structure.require(
                scenarioArgs.length == 0
                        || (scenarioArgs.length == 2 && scenarioArgs[0].equals("--scenario")),
                "Use [--attach] [--scenario a,b]");
        var requested =
                (scenarioArgs.length == 0 ? Names.SCENARIOS : List.of(scenarioArgs[1].split(",", -1)))
                        .stream().map(Scenario::parse).toList();
        Structure.require(
                !requested.isEmpty() && new HashSet<>(requested).size() == requested.size(),
                "Invalid scenarios");
        var selected = Arrays.stream(Scenario.values()).filter(requested::contains).toList();
        if (attach) {
            Structure.require(
                    !selected.contains(Scenario.UPDATE),
                    "The update scenario requires a fresh server with capacity set for its test run.");
            var server = ServerControl.attach(root);
            System.out.println("Attached to Session STREAM: " + server.endpoint());
            if (!run(selected, server)) System.exit(1);
            return;
        }
        boolean passed;
        try (var host = new ServerHost(root, selected.size(), false)) {
            System.out.println("Logs: " + host.logs);
            passed = run(selected, host.start());
        }
        System.exit(passed ? 0 : 1);
    }

    private static boolean run(List<Scenario> selected, ServerControl.Connection server) {
        int passed = 0;
        for (int index = 0; index < selected.size(); index++) {
            var id = selected.get(index);
            System.out.printf("━━ %s (%d/%d) ━━%n", id.id(), index + 1, selected.size());
            try {
                Scenarios.run(id, server);
                passed++;
                System.out.println("✓ " + id.id());
            } catch (Exception error) {
                System.out.println("✗ " + id.id());
                error.printStackTrace(System.err);
            }
        }
        System.out.println("Scenarios: " + passed + "/" + selected.size() + " passed");
        return passed == selected.size();
    }
}
