package systems.zlink.e2e.spotservice.client.Scenarios;

public final class SmE2Scenario {
    private SmE2Scenario() {
    }

    public static void run(SpotServiceScenarioContext context) {
        SmA1Scenario.run(context);
    }
}
