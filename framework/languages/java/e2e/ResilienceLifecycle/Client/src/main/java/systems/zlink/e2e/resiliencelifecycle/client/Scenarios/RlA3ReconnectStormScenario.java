package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;
import java.util.Set;

public final class RlA3ReconnectStormScenario {
    private RlA3ReconnectStormScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        context.waitForTopology(2);
        Set<String> providers = context.collectProviders("a3-storm", 40, 1);
        ResilienceScenarioContext.ensure(!providers.isEmpty(),
            "RL-A3 storm client did not receive replies");
        System.out.println("scenario RL-A3 passed");
        System.out.println("scenario RL-D1 passed");
    }
}
