package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA2Scenario extends SpotServiceScenarioContext {
    private SmA2Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmA2Scenario(context).execute();
    }

    private void execute() {
        Contracts.StateRes second = eventually(() -> requestState("room-a", "a2", REQUEST_TIMEOUT));
        ensure(second.value().contains("a1") && second.value().contains("a2"),
            "SM-A2 state did not accumulate");
        System.out.println("scenario SM-A2 passed");

    }
}
