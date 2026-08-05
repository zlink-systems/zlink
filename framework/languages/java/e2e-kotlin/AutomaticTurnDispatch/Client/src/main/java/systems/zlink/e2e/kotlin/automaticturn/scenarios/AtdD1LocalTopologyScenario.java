package systems.zlink.e2e.kotlin.automaticturn.scenarios;

import java.util.Collection;
import systems.zlink.stream.connector.ZLinkStreamConnector;

public final class AtdD1LocalTopologyScenario {
    private static final String[] REQUIRED_LOCAL_SCENARIOS = {
        "ATD-A1",
        "ATD-A2",
        "ATD-A3",
        "ATD-A4",
        "ATD-B1",
        "ATD-B2",
        "ATD-B3",
        "ATD-C1",
        "ATD-C2",
        "ATD-C3",
        "ATD-D4",
        "ATD-E1"
    };

    private AtdD1LocalTopologyScenario() {
    }

    public static void run(ZLinkStreamConnector connector, Collection<String> completedScenarios) {
        if (connector == null) {
            throw new IllegalArgumentException("connector is required");
        }
        for (String scenario : REQUIRED_LOCAL_SCENARIOS) {
            if (!completedScenarios.contains(scenario)) {
                throw new IllegalStateException("ATD-D1 missing local topology scenario marker: " + scenario);
            }
        }
        System.out.println("scenario ATD-D1 passed");
    }
}
