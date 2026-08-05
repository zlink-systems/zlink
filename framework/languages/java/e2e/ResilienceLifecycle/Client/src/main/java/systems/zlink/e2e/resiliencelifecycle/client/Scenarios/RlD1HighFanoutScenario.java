package systems.zlink.e2e.resiliencelifecycle.client.Scenarios;

import systems.zlink.e2e.resiliencelifecycle.client.Support.ResilienceScenarioContext;

public final class RlD1HighFanoutScenario {
    private RlD1HighFanoutScenario() {
    }

    public static void run(ResilienceScenarioContext context) {
        RlA3ReconnectStormScenario.run(context);
    }
}
