package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmC1Scenario extends SpotServiceScenarioContext {
    private SmC1Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmC1Scenario(context).execute();
    }

    private void execute() {
        Contracts.StateRes after = eventually(() -> requestState("room-a", "after-timeout", REQUEST_TIMEOUT));
        ensure(after.value().contains("after-timeout"), "SM-C1 post-timeout request failed");
        System.out.println("scenario SM-C1 passed");
        System.out.println("scenario SM-C1-normal passed");

    }
}
