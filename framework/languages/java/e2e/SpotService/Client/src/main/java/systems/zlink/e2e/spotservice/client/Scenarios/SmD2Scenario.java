package Scenarios;

import systems.zlink.e2e.spotservice.client.Scenarios;
public final class SmD2Scenario {
    private SmD2Scenario() {
    }

    public static void run(SpotServiceScenarioContext context) {
        SmB2Scenario.run(context);
    }
}
