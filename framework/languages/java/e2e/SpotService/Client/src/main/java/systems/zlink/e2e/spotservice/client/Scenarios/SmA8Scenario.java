package systems.zlink.e2e.spotservice.client.Scenarios;

import systems.zlink.e2e.spotservice.shared.Contracts;

public final class SmA8Scenario extends SpotServiceScenarioContext {
    private SmA8Scenario(SpotServiceScenarioContext context) {
        super(context);
    }

    public static void run(SpotServiceScenarioContext context) {
        new SmA8Scenario(context).execute();
    }

    private void execute() {
        eventually(() -> requestState("room-a", "worker-start", REQUEST_TIMEOUT));
        Contracts.StateRes followUp = eventually(() -> requestState("room-a", "worker-follow-up", REQUEST_TIMEOUT));
        ensure(followUp.value().contains("worker-follow-up"),
            "SM-A8 follow-up state was not applied");
        System.out.println("scenario SM-A8 passed");

    }
}
