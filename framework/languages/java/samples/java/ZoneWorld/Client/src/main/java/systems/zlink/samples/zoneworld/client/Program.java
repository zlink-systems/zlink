package systems.zlink.samples.zoneworld.client;

import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.Set;

public final class Program {
    private Program() {}

    public static void main(String[] args) throws Exception {
        ClientOptions options = ClientOptions.load(args);
        Set<String> selected = new LinkedHashSet<>();
        if ("all".equalsIgnoreCase(options.scenarios())) {
            selected.addAll(Scenarios.clientDriven().keySet());
        } else {
            selected.addAll(Arrays.asList(options.scenarios().split(",")));
        }
        for (String id : selected) {
            Scenarios.Scenario scenario = Scenarios.clientDriven().get(id);
            if (scenario == null) scenario = Scenarios.runnerDriven().get(id);
            if (scenario == null) throw new IllegalArgumentException("unknown scenario: " + id);
            scenario.run(options);
            System.out.println("scenario " + id + " passed");
        }
    }
}
